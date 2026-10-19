#include "powermode.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <Ticker.h>
extern "C" {
#include <user_interface.h>
}

#if __has_include("config.h")
#include "config.h"
#else
#error "src/config.h is missing. Copy src/config.h.example to src/config.h and fill it in."
#endif

#include "applog.h"
#include "evlog.h"
#include "ha_mqtt.h"
#include "voltguard.h"

#ifndef POWER_DEFAULT_MODE
#define POWER_DEFAULT_MODE "normal" // what a fresh board does: storage, normal or high
#endif
#ifndef POWER_HIGH_PUBLISH_S
#define POWER_HIGH_PUBLISH_S 120
#endif
#ifndef POWER_HIGH_POLL_S
// The BMS is polled this often; a report follows each sweep (ha_mqtt.cpp). Near the
// guard's trip level it polls at POLL_INTERVAL_MS instead (main.cpp).
#define POWER_HIGH_POLL_S 120
#endif
#ifndef POWER_HIGH_LIGHT_SLEEP
// 1 lets the CPU sleep between radio events, once MQTT has settled (main.cpp).
#define POWER_HIGH_LIGHT_SLEEP 1
#endif
#ifndef POWER_HIGH_KEEPALIVE_S
// Announced in CONNECT, so the broker marks a dead board offline after 1.5x this. The
// bridge pings at half of it, which also keeps a reverse proxy from closing the socket.
#define POWER_HIGH_KEEPALIVE_S 60
#endif
#ifndef POWER_SLEEP_CLOCK_PERMILLE
// The chip's sleep timer runs fast, so a planned sleep takes less real time than it
// says. Uptime counts a planned sleep scaled by this, per thousand. It was tuned against
// a real clock over a long storage run; adjust it the same way if uptime drifts.
#define POWER_SLEEP_CLOCK_PERMILLE 940
#endif
#ifndef POWER_NORMAL_INTERVAL_S
// Normal deep-sleeps between reports, like storage but on a short cycle: it wakes,
// connects, reports one fresh BMS sweep and sleeps again. The BMS is only polled
// during the session, so its own sleep timeout no longer matters.
#define POWER_NORMAL_INTERVAL_S 900
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
#ifndef POWER_OFFLINE_GIVEUP_S
// Without WiFi for this long, every mode drops to POWER_OFFLINE_INTERVAL_S between
// network tests, so a bridge stored out of range does not drain its own pack.
#define POWER_OFFLINE_GIVEUP_S 3600
#endif
#ifndef POWER_OFFLINE_INTERVAL_S
#define POWER_OFFLINE_INTERVAL_S 86400
#endif
#ifndef POWER_STORAGE_GRACE_S
#define POWER_STORAGE_GRACE_S 120
#endif

namespace powermode {
namespace {

using powercore::ChainRecord;
using powercore::ModeRecord;

constexpr uint32_t kRtcBlock = 72; // the guard owns 64..66, see voltguard.cpp
constexpr uint32_t kRtcUptimeBlock = 80; // uptime total across deep sleeps
constexpr uint32_t kSessionTimeoutMs = 90000;
constexpr uint32_t kRtcOfflineBlock = 88; // time without WiFi, across sleeps and restarts
constexpr uint32_t kEepromBytes = 16;

// Keep-alive pings are what stop a reverse proxy from closing an idle socket
// (nginx defaults to 60 s). ha_mqtt.cpp announces a keepalive of twice the ping
// interval, never below 60 s.
constexpr Settings kHigh = {
    static_cast<uint32_t>(POWER_HIGH_PUBLISH_S) * 1000UL,
    static_cast<uint32_t>(POWER_HIGH_POLL_S) * 1000UL,
    static_cast<uint32_t>(POWER_HIGH_POLL_S) * 3000UL, // three missed sweeps
    static_cast<uint32_t>(POWER_HIGH_KEEPALIVE_S) * 500UL, POWER_HIGH_LIGHT_SLEEP != 0, 0};

// A normal or storage boot is a short high-mode session: it wants BMS data quickly
// and publishes exactly once, so the publish interval is effectively never.
constexpr Settings kSession = {0xFFFFFFFFUL, POLL_INTERVAL_MS, STALE_AFTER_MS, 20000,
                               false, 0};

Mode mode_ = Mode::Normal;
bool have_pending_ = false;
Mode pending_ = Mode::Normal;

uint32_t uptime_base_s_ = 0; // total from before this boot, carried across deep sleeps
uint32_t uptime_last_ms_ = 0;
uint64_t uptime_run_ms_ = 0; // this boot, kept in 64 bits so millis() rolling over does not matter

bool woke_for_report_ = false; // this boot is the end of a storage sleep
uint32_t grace_ms_ = 0;     // how long a storage boot waits after its report
uint32_t reported_at_ = 0;  // millis() when the report completed
bool reported_ = false;

// --- time without WiFi ------------------------------------------------------

uint32_t offline_base_s_ = 0;    // total from before this boot, if a record was left
bool offline_record_ = false;    // a record is sitting in RTC memory
bool wifi_down_ = true;          // no WiFi yet this boot counts as down, from boot
uint32_t wifi_down_since_ms_ = 0;

// Time without WiFi up to now, 0 while connected.
uint32_t offlineS() {
    return wifi_down_ ? offline_base_s_ + (millis() - wifi_down_since_ms_) / 1000UL : 0;
}

void writeOffline(uint32_t down_s) {
    powercore::OfflineRecord r = powercore::makeOfflineRecord(down_s);
    ESP.rtcUserMemoryWrite(kRtcOfflineBlock, reinterpret_cast<uint32_t*>(&r), sizeof(r));
    offline_record_ = true;
}

// --- storage sleep chain ----------------------------------------------------

// ESP.deepSleep() only schedules the sleep and suspends the sketch; the SDK should stop the
// chip a few milliseconds later. After a session that ran with WiFi up it has been seen not
// to: the chip stays awake with the radio on and answers ping, but the sketch never runs
// again, so no watchdog fires. This timer lives in the SDK and still fires then. It
// retries the sleep from a fresh boot, which has no WiFi and sleeps reliably.
constexpr uint32_t kSleepGuardMs = 3000;
Ticker sleep_guard_;
uint32_t sleep_request_s_ = 0; // what sleepChain() was asked for, this chunk and the rest

void sleepFailed() {
    evlog::add(evlog::SleepFailed);
    // sleepChain() credited the sleep to the totals; it did not happen, and the retry
    // credits it again.
    powercore::UptimeRecord u = powercore::makeUptimeRecord(uptimeS());
    ESP.rtcUserMemoryWrite(kRtcUptimeBlock, reinterpret_cast<uint32_t*>(&u), sizeof(u));
    if (offline_record_) writeOffline(offlineS());
    ChainRecord r = powercore::makeChainRecord(sleep_request_s_);
    ESP.rtcUserMemoryWrite(kRtcBlock, reinterpret_cast<uint32_t*>(&r), sizeof(r));
    ESP.restart();
}

[[noreturn]] void sleepChain(uint32_t seconds) {
    const powercore::SleepPlan p = powercore::planSleep(seconds, ESP.deepSleepMax());
    sleep_request_s_ = seconds;
    // Park the running total, with this sleep added, for the next boot to pick up.
    powercore::UptimeRecord u = powercore::makeUptimeRecord(
        uptimeS() + static_cast<uint32_t>(p.sleep_us / 1000000ULL * POWER_SLEEP_CLOCK_PERMILLE / 1000ULL));
    ESP.rtcUserMemoryWrite(kRtcUptimeBlock, reinterpret_cast<uint32_t*>(&u), sizeof(u));
    // Credited chunk by chunk, scaled like the uptime's, so a reset that cancels the
    // rest of the chain does not leave hours on the total that were never slept. A
    // record exists only while WiFi is missing (storageSleep, or an earlier boot).
    if (offline_record_) {
        writeOffline(offlineS() + static_cast<uint32_t>(p.sleep_us / 1000000ULL *
                                                         POWER_SLEEP_CLOCK_PERMILLE / 1000ULL));
    }
    ChainRecord r = powercore::makeChainRecord(p.remaining_after_s);
    ESP.rtcUserMemoryWrite(kRtcBlock, reinterpret_cast<uint32_t*>(&r), sizeof(r));

    // WAKE_RF_DEFAULT, not WAKE_RF_DISABLED, for the same reason as in
    // voltguard.cpp: the disabled flag outlives the sleep and a later restart
    // would come back with no radio at all. A few hundred ms of RF calibration
    // every hour or so is nothing next to that.
    sleep_guard_.once_ms(kSleepGuardMs, sleepFailed);
    ESP.deepSleep(p.sleep_us, WAKE_RF_DEFAULT);
    while (true) delay(100);
}

void clearChain() {
    ChainRecord r{0, 0, 0};
    ESP.rtcUserMemoryWrite(kRtcBlock, reinterpret_cast<uint32_t*>(&r), sizeof(r));
}

[[noreturn]] void storageSleep(uint32_t seconds, const char* why) {
    APPLOG("power: storage sleep %us (%s)", static_cast<unsigned>(seconds), why);
    // Still no WiFi: from here sleepChain() counts each chunk toward giving up.
    if (wifi_down_) offline_record_ = true;
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

bool saveMode(Mode m) {
    ModeRecord r = powercore::makeModeRecord(m);
    EEPROM.put(0, r);
    if (EEPROM.commit()) return true;
    APPLOG("power: could not save mode to flash");
    return false;
}

} // namespace

void sleepIfMidChain() {
    const bool woke = ESP.getResetInfoPtr()->reason == REASON_DEEP_SLEEP_AWAKE;

    // Uptime carries over whatever left a record behind: a deep sleep, or the restart a
    // mode change makes. A power-up leaves RTC memory empty, and the record is cleared
    // once read, so a crash or an OTA restart later starts it again from zero.
    powercore::UptimeRecord u;
    ESP.rtcUserMemoryRead(kRtcUptimeBlock, reinterpret_cast<uint32_t*>(&u), sizeof(u));
    if (powercore::readUptimeRecord(u, uptime_base_s_)) {
        u = powercore::UptimeRecord{0, 0, 0};
        ESP.rtcUserMemoryWrite(kRtcUptimeBlock, reinterpret_cast<uint32_t*>(&u), sizeof(u));
    }

    // Kept, not cleared: the total stays until a connection ends it (noteWifi).
    powercore::OfflineRecord o;
    ESP.rtcUserMemoryRead(kRtcOfflineBlock, reinterpret_cast<uint32_t*>(&o), sizeof(o));
    offline_record_ = powercore::readOfflineRecord(o, offline_base_s_);
    if (!offline_record_) offline_base_s_ = 0;

    ChainRecord r;
    ESP.rtcUserMemoryRead(kRtcBlock, reinterpret_cast<uint32_t*>(&r), sizeof(r));
    uint32_t remaining = 0;
    if (!powercore::readChainRecord(r, remaining)) return;

    // Whatever happens next, this record has been used. A stale one must never be
    // believed twice.
    clearChain();

    // A reset button pressed mid-chain is not a wake we can interpret. A software restart
    // is: sleepFailed() makes one, with the whole sleep still to do.
    const bool retry = ESP.getResetInfoPtr()->reason == REASON_SOFT_RESTART;
    if (!woke && !retry) return;

    if (remaining == 0) {
        if (woke) woke_for_report_ = true;
        return;
    }
    sleepChain(remaining);
}

void begin() {
    mode_ = loadMode();
    // A sleeping-mode boot that is the end of a sleep reports and goes back. Any
    // other one (power-up, reset, a mode change just made) holds still for a while
    // first, so there is a window to change course.
    grace_ms_ = (sleeps() && !woke_for_report_)
                    ? static_cast<uint32_t>(POWER_STORAGE_GRACE_S) * 1000UL
                    : 0;
    APPLOG("power: mode %s%s", powercore::modeName(mode_),
                 woke_for_report_ ? " (scheduled report)" : "");
}

uint32_t uptimeS() {
    // service() calls this every pass, so millis() cannot wrap unseen.
    const uint32_t now = millis();
    uptime_run_ms_ += static_cast<uint32_t>(now - uptime_last_ms_);
    uptime_last_ms_ = now;
    return uptime_base_s_ + static_cast<uint32_t>(uptime_run_ms_ / 1000ULL);
}

Mode mode() { return mode_; }

const char* modeName() { return powercore::modeName(mode_); }

bool sleeps() { return mode_ == Mode::Normal || mode_ == Mode::Storage; }

const Settings& settings() { return mode_ == Mode::High ? kHigh : kSession; }

uint32_t sleepIntervalS() {
    return mode_ == Mode::Normal ? POWER_NORMAL_INTERVAL_S : POWER_STORAGE_INTERVAL_S;
}

void noteWifi(bool up) {
    if (up) {
        if (offline_record_) {
            powercore::OfflineRecord r{0, 0, 0};
            ESP.rtcUserMemoryWrite(kRtcOfflineBlock, reinterpret_cast<uint32_t*>(&r), sizeof(r));
            offline_record_ = false;
        }
        offline_base_s_ = 0;
        wifi_down_ = false;
    } else if (!wifi_down_) {
        // A drop after being up: the count starts here, from nothing.
        wifi_down_ = true;
        wifi_down_since_ms_ = millis();
    }
}

void saveOffline() {
    if (wifi_down_) writeOffline(offlineS());
}

void request(Mode m) {
    pending_ = m;
    have_pending_ = true;
}

bool service() {
    uptimeS(); // keeps the millis() roll-over accounting current, whoever asks for uptime

    // A BMS write that has been accepted but not finished must not be cut off by the
    // restart a mode change makes; the request waits for the next pass.
    if (have_pending_ && !ha_mqtt::writePending()) {
        have_pending_ = false;
        if (pending_ != mode_) {
            const Mode old = mode_;
            APPLOG("power: mode %s -> %s", powercore::modeName(old),
                         powercore::modeName(pending_));
            // Unsaved, the change would be lost at the restart below and a retained
            // command would be applied again on the next boot, forever.
            if (!saveMode(pending_)) {
                APPLOG("power: mode change dropped");
                return false;
            }
            mode_ = pending_;

            // The radio, the CPU and the whole session logic differ between high
            // and the sleeping modes. A clean start is much easier to trust than a
            // live transition.
            powercore::UptimeRecord u = powercore::makeUptimeRecord(uptimeS());
            ESP.rtcUserMemoryWrite(kRtcUptimeBlock, reinterpret_cast<uint32_t*>(&u), sizeof(u));
            ha_mqtt::goodbye();
            delay(100);
            ESP.restart();
        }
    }

    if (sleeps()) {
        // The grace window runs from the report, not from boot: a slow WiFi/TLS
        // connect would otherwise leave almost none of it.
        if (!reported_ && ha_mqtt::sessionComplete()) {
            reported_ = true;
            reported_at_ = millis();
        }
        // A guard countdown needs this session to last until it trips or stands down.
        if (voltguard::status().state == voltguard::State::Counting) return false;
        // Same for a guard change (a stand-down, say) still waiting to be published.
        if (ha_mqtt::reportPending()) return false;
        if (reported_) {
            if (millis() - reported_at_ >= grace_ms_ && !ha_mqtt::writePending()) {
                storageSleep(sleepIntervalS(), "reported");
            }
        } else if (millis() >= kSessionTimeoutMs && !ha_mqtt::writePending()) {
            // Retry sooner than a day for storage, but never later than the cycle.
            const uint32_t retry = POWER_STORAGE_RETRY_S;
            const uint32_t cycle = sleepIntervalS() < retry ? sleepIntervalS() : retry;
            // offlineS() is 0 while WiFi is up (broker trouble), so only a missing network
            // can reach the give-up point.
            storageSleep(powercore::offlineSleepS(offlineS(), cycle, POWER_OFFLINE_GIVEUP_S,
                                                  POWER_OFFLINE_INTERVAL_S),
                         "no report got through");
        }
    } else if (powercore::offlineGiveUp(offlineS(), POWER_OFFLINE_GIVEUP_S) &&
               millis() >= kSessionTimeoutMs && !ha_mqtt::writePending() &&
               voltguard::status().state != voltguard::State::Counting) {
        // High never sleeps, so without WiFi it would hunt for a network round the
        // clock. Past the give-up point it sleeps like the others. The 90 s wait is a
        // test of its own: every wake starts with the total still counted.
        storageSleep(POWER_OFFLINE_INTERVAL_S, "no wifi");
    }
    return false;
}

} // namespace powermode
