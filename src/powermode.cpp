#include "powermode.h"

#include <Arduino.h>
#include <EEPROM.h>
extern "C" {
#include <user_interface.h>
}

#if __has_include("config.h")
#include "config.h"
#else
#error "src/config.h is missing. Copy src/config.h.example to src/config.h and fill it in."
#endif

#include "applog.h"
#include "ha_mqtt.h"

#ifndef POWER_DEFAULT_MODE
#define POWER_DEFAULT_MODE "normal" // what a fresh board does: storage, normal or high
#endif
#ifndef POWER_HIGH_PUBLISH_S
#define POWER_HIGH_PUBLISH_S 60
#endif
#ifndef POWER_NORMAL_PUBLISH_S
#define POWER_NORMAL_PUBLISH_S 900
#endif
#ifndef POWER_NORMAL_POLL_S
// The low-voltage guard needs readings to keep coming, so normal mode still polls
// the BMS between reports. Each sweep keeps the CPU awake for about half a second.
// While the guard is counting down it polls at POLL_INTERVAL_MS instead (main.cpp),
// so this can be longer than GUARD_TRIP_SECONDS.
#define POWER_NORMAL_POLL_S 60
#endif
#ifndef POWER_NORMAL_LISTEN_INTERVAL
// Beacons (about 100 ms each) between radio wakeups, 1..10. Only takes effect on
// NONOS SDK 3.x (build flag -D PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK305); the
// default SDK 2.2.x ignores it and wakes at every DTIM.
#define POWER_NORMAL_LISTEN_INTERVAL 10
#endif
#ifndef POWER_NORMAL_LIGHT_SLEEP
// 1 lets the CPU sleep between radio events, once MQTT has settled (main.cpp).
#define POWER_NORMAL_LIGHT_SLEEP 1
#endif
#ifndef POWER_STORAGE_INTERVAL_S
#define POWER_STORAGE_INTERVAL_S 86400
#endif
#ifndef POWER_STORAGE_RETRY_S
// When a daily report could not get through, try again after this long rather
// than waiting out another day -- but not every few minutes either, since each
// attempt spends up to a minute with the radio hunting for a network.
#define POWER_STORAGE_RETRY_S 10800
#endif
#ifndef POWER_STORAGE_GRACE_S
#define POWER_STORAGE_GRACE_S 120
#endif

namespace powermode {
namespace {

using powercore::ChainRecord;
using powercore::ModeRecord;

constexpr uint32_t kRtcBlock = 72; // the guard owns 64..66, see voltguard.cpp
constexpr uint32_t kSessionTimeoutMs = 90000;
constexpr uint32_t kEepromBytes = 16;

// Keep-alive pings are what stop a reverse proxy from closing an idle socket
// (nginx defaults to 60 s). Normal can afford to ping less often than high.
constexpr Settings kHigh = {
    static_cast<uint32_t>(POWER_HIGH_PUBLISH_S) * 1000UL, POLL_INTERVAL_MS, STALE_AFTER_MS,
    20000, false, 0};

constexpr Settings kNormal = {
    static_cast<uint32_t>(POWER_NORMAL_PUBLISH_S) * 1000UL,
    static_cast<uint32_t>(POWER_NORMAL_POLL_S) * 1000UL,
    static_cast<uint32_t>(POWER_NORMAL_POLL_S) * 3000UL, // three missed sweeps
    45000, POWER_NORMAL_LIGHT_SLEEP != 0, POWER_NORMAL_LISTEN_INTERVAL};

// A storage boot is a short high-mode session: it wants BMS data quickly and
// publishes exactly once, so the publish interval is effectively never.
constexpr Settings kStorageSession = {0xFFFFFFFFUL, POLL_INTERVAL_MS, STALE_AFTER_MS, 20000,
                                      false, 0};

Mode mode_ = Mode::Normal;
bool have_pending_ = false;
Mode pending_ = Mode::Normal;

bool woke_for_report_ = false; // this boot is the end of a storage sleep
uint32_t grace_ms_ = 0;

// --- storage sleep chain ----------------------------------------------------

[[noreturn]] void sleepChain(uint32_t seconds) {
    const powercore::SleepPlan p = powercore::planSleep(seconds, ESP.deepSleepMax());
    ChainRecord r = powercore::makeChainRecord(p.remaining_after_s);
    ESP.rtcUserMemoryWrite(kRtcBlock, reinterpret_cast<uint32_t*>(&r), sizeof(r));

    // WAKE_RF_DEFAULT, not WAKE_RF_DISABLED, for the same reason as in
    // voltguard.cpp: the disabled flag outlives the sleep and a later restart
    // would come back with no radio at all. A few hundred ms of RF calibration
    // every hour or so is nothing next to that.
    ESP.deepSleep(p.sleep_us, WAKE_RF_DEFAULT);
    while (true) delay(100);
}

void clearChain() {
    ChainRecord r{0, 0, 0};
    ESP.rtcUserMemoryWrite(kRtcBlock, reinterpret_cast<uint32_t*>(&r), sizeof(r));
}

[[noreturn]] void storageSleep(uint32_t seconds, const char* why) {
    APPLOG("power: storage sleep %us (%s)", static_cast<unsigned>(seconds), why);
    ha_mqtt::goodbye();
    delay(100);
    sleepChain(seconds);
}

// --- persistence ------------------------------------------------------------

Mode loadMode() {
    Mode fallback = Mode::Normal;
    powercore::parseMode(POWER_DEFAULT_MODE, strlen(POWER_DEFAULT_MODE), fallback);

    EEPROM.begin(kEepromBytes);
    ModeRecord r;
    EEPROM.get(0, r);
    Mode m;
    return powercore::readModeRecord(r, m) ? m : fallback;
}

void saveMode(Mode m) {
    ModeRecord r = powercore::makeModeRecord(m);
    EEPROM.put(0, r);
    if (!EEPROM.commit()) APPLOG("power: could not save mode to flash");
}

} // namespace

void sleepIfMidChain() {
    const bool woke = ESP.getResetInfoPtr()->reason == REASON_DEEP_SLEEP_AWAKE;

    ChainRecord r;
    ESP.rtcUserMemoryRead(kRtcBlock, reinterpret_cast<uint32_t*>(&r), sizeof(r));
    uint32_t remaining = 0;
    if (!powercore::readChainRecord(r, remaining)) return;

    // Whatever happens next, this record has been used. A stale one must never be
    // believed twice.
    clearChain();

    // A reset button pressed mid-chain is not a wake we can interpret.
    if (!woke) return;

    if (remaining == 0) {
        woke_for_report_ = true;
        return;
    }
    sleepChain(remaining);
}

void begin() {
    mode_ = loadMode();
    // A storage boot that is the end of a sleep reports and goes back. Any other
    // storage boot (power-up, reset, a mode change just made) holds still for a
    // while first, so there is a window to change course.
    grace_ms_ = (mode_ == Mode::Storage && !woke_for_report_)
                    ? static_cast<uint32_t>(POWER_STORAGE_GRACE_S) * 1000UL
                    : 0;
    APPLOG("power: mode %s%s", powercore::modeName(mode_),
                 woke_for_report_ ? " (daily report)" : "");
}

Mode mode() { return mode_; }

const char* modeName() { return powercore::modeName(mode_); }

const Settings& settings() {
    switch (mode_) {
    case Mode::High: return kHigh;
    case Mode::Storage: return kStorageSession;
    default: return kNormal;
    }
}

uint32_t storageIntervalS() { return POWER_STORAGE_INTERVAL_S; }

void request(Mode m) {
    pending_ = m;
    have_pending_ = true;
}

bool service() {
    bool changed = false;

    if (have_pending_) {
        have_pending_ = false;
        if (pending_ != mode_) {
            const Mode old = mode_;
            APPLOG("power: mode %s -> %s", powercore::modeName(old),
                         powercore::modeName(pending_));
            saveMode(pending_);
            mode_ = pending_;

            if (old == Mode::Storage || mode_ == Mode::Storage) {
                // The radio, the CPU and the whole session logic differ. A clean
                // start is much easier to trust than a live transition.
                ha_mqtt::goodbye();
                delay(100);
                ESP.restart();
            }
            changed = true; // normal <-> high is just different numbers
        }
    }

    if (mode_ == Mode::Storage && millis() >= grace_ms_) {
        if (ha_mqtt::sessionComplete()) {
            storageSleep(POWER_STORAGE_INTERVAL_S, "reported");
        } else if (millis() >= kSessionTimeoutMs) {
            storageSleep(POWER_STORAGE_RETRY_S, "no report got through");
        }
    }
    return changed;
}

} // namespace powermode
