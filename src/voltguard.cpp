#include "voltguard.h"

#include <ESP8266WiFi.h>

#if __has_include("config.h")
#include "config.h"
#else
#error "src/config.h is missing. Copy src/config.h.example to src/config.h and fill it in."
#endif

#include "applog.h"

namespace voltguard {
namespace {

Status status_;
uint32_t low_since_ = 0;
uint32_t recheck_deadline_ = 0;

#if GUARD_ENABLE

// --- state that has to survive deep sleep -----------------------------------
//
// RTC user memory is 512 bytes (128 four-byte blocks) and keeps its contents
// through deep sleep, unlike RAM. The low blocks are used by the core's WiFi
// calibration stash on some builds, so sit well clear of them.
constexpr uint32_t kRtcBlock = 64;
constexpr uint32_t kMagic = 0x47554131UL; // "GUA1"

struct RtcState {
    uint32_t magic;
    uint16_t sleeps;
    uint16_t trip_mv;
    uint32_t check;
};

// Not a real CRC. RTC memory only has to survive a controlled sleep, not a
// hostile world -- this catches uninitialised memory, which is the only failure
// mode that matters here.
uint32_t checkOf(const RtcState& s) {
    return s.magic ^ (static_cast<uint32_t>(s.sleeps) << 16) ^ s.trip_mv ^ 0xA5A5A5A5UL;
}

bool readRtc(RtcState& s) {
    if (!ESP.rtcUserMemoryRead(kRtcBlock, reinterpret_cast<uint32_t*>(&s), sizeof(s))) return false;
    return s.magic == kMagic && s.check == checkOf(s);
}

void writeRtc(uint16_t sleeps, uint16_t trip_mv) {
    RtcState s{kMagic, sleeps, trip_mv, 0};
    s.check = checkOf(s);
    ESP.rtcUserMemoryWrite(kRtcBlock, reinterpret_cast<uint32_t*>(&s), sizeof(s));
}

void clearRtc() {
    RtcState s{0, 0, 0, 0};
    ESP.rtcUserMemoryWrite(kRtcBlock, reinterpret_cast<uint32_t*>(&s), sizeof(s));
}

// --- measurement ------------------------------------------------------------

// Below either threshold counts as low. The weakest cell is what actually gets
// damaged, but it is only known once 0x91 has answered, so the pack total is
// kept as a fallback for a partly-populated struct.
bool below(const daly::BmsData& d, uint16_t cell_mv, uint16_t pack_dv) {
    if (d.min_cell_mv > 0 && d.min_cell_mv <= cell_mv) return true;
    if (d.pack_voltage_dv > 0 && d.pack_voltage_dv <= pack_dv) return true;
    return false;
}

// Where the BMS itself gives up. Its two levels are used as reported, taking the
// higher so we are clear of the earlier one. A reply that is zero or implausible
// (an old firmware answering with something else entirely) is ignored and the
// configured thresholds apply.
constexpr uint16_t kMinPlausibleCellMv = 2000;
constexpr uint16_t kMaxPlausibleCellMv = 3600;

uint16_t bmsCellLimitMv(const daly::BmsData& d) {
    const uint16_t lim = d.cell_low_l1_mv > d.cell_low_l2_mv ? d.cell_low_l1_mv : d.cell_low_l2_mv;
    return lim >= kMinPlausibleCellMv && lim <= kMaxPlausibleCellMv ? lim : 0;
}

uint16_t bmsPackLimitDv(const daly::BmsData& d, uint16_t cell_limit_mv) {
    const uint16_t lim = d.pack_low_l1_dv > d.pack_low_l2_dv ? d.pack_low_l1_dv : d.pack_low_l2_dv;
    if (lim == 0 || d.cell_count == 0) return 0;
    // The pack limit has to be consistent with the cell limit or one of them
    // was misread; trust neither the pack number nor the cell number alone then.
    const uint32_t expect_dv = static_cast<uint32_t>(cell_limit_mv) * d.cell_count / 100U;
    return lim * 10U >= expect_dv * 8U && lim * 10U <= expect_dv * 12U ? lim : 0;
}

struct Thresholds {
    uint16_t trip_cell_mv;
    uint16_t trip_pack_dv;
    uint16_t release_cell_mv;
    uint16_t release_pack_dv;
    bool from_bms;
};

Thresholds thresholds(const daly::BmsData& d) {
    const uint16_t cell = bmsCellLimitMv(d);
    const uint16_t pack = cell ? bmsPackLimitDv(d, cell) : 0;
    if (cell == 0 || pack == 0) {
        return {GUARD_TRIP_CELL_MV, GUARD_TRIP_PACK_DV, GUARD_RELEASE_CELL_MV,
                GUARD_RELEASE_PACK_DV, false};
    }
    const uint16_t margin_dv = static_cast<uint16_t>(GUARD_BMS_MARGIN_MV * d.cell_count / 100U);
    const uint16_t hyst_dv = static_cast<uint16_t>(GUARD_BMS_RELEASE_MV * d.cell_count / 100U);
    return {static_cast<uint16_t>(cell + GUARD_BMS_MARGIN_MV),
            static_cast<uint16_t>(pack + margin_dv),
            static_cast<uint16_t>(cell + GUARD_BMS_MARGIN_MV + GUARD_BMS_RELEASE_MV),
            static_cast<uint16_t>(pack + margin_dv + hyst_dv), true};
}

bool belowTrip(const daly::BmsData& d) {
    const Thresholds t = thresholds(d);
    return below(d, t.trip_cell_mv, t.trip_pack_dv);
}

bool belowRelease(const daly::BmsData& d) {
    const Thresholds t = thresholds(d);
    return below(d, t.release_cell_mv, t.release_pack_dv);
}

// --- actions ----------------------------------------------------------------

[[noreturn]] void sleepNow(const daly::BmsData& d) {
    const uint16_t trip_mv = d.min_cell_mv > 0 ? d.min_cell_mv : status_.trip_mv;
    const uint16_t sleeps = status_.sleeps + 1;
    writeRtc(sleeps, trip_mv);

    uint64_t us = static_cast<uint64_t>(GUARD_SLEEP_MINUTES) * 60ULL * 1000000ULL;
    if (us > ESP.deepSleepMax()) us = ESP.deepSleepMax();

    APPLOG("guard: sleeping %lus, cell %umV pack %u.%uV (#%u)",
                 static_cast<unsigned long>(us / 1000000ULL), trip_mv,
                 d.pack_voltage_dv / 10, d.pack_voltage_dv % 10, sleeps);

    // The log lives in RAM and dies with the sleep, so give a dashboard that is
    // mid-poll its last answer before the lights go out.
    delay(200);

    // WAKE_RF_DEFAULT, not WAKE_RF_DISABLED. The disabled flag persists in RTC
    // until the next RF-enabled wake, so a later ESP.restart() would come back
    // with no radio at all and no way to ask for one. Paying ~200 ms of RF init
    // every GUARD_SLEEP_MINUTES is far cheaper than that failure mode; the guard
    // shuts the radio down again immediately in begin().
    ESP.deepSleep(us, WAKE_RF_DEFAULT);

    // deepSleep() schedules the sleep and returns; the chip takes a moment to
    // actually go. Nothing after this point runs.
    while (true) delay(100);
}

[[noreturn]] void resumeNormal(const daly::BmsData& d) {
    APPLOG("guard: recovered at cell %umV pack %u.%uV after %u sleeps, resuming",
                 d.min_cell_mv, d.pack_voltage_dv / 10, d.pack_voltage_dv % 10, status_.sleeps);
    clearRtc();
    delay(100);
    ESP.restart();
    while (true) delay(100);
}

#endif // GUARD_ENABLE

} // namespace

bool begin() {
#if !GUARD_ENABLE
    status_.state = State::Disabled;
    APPLOG("guard: disabled");
    return false;
#else
    RtcState s;
    const bool woke_from_sleep =
        ESP.getResetInfoPtr()->reason == REASON_DEEP_SLEEP_AWAKE && readRtc(s);

    if (!woke_from_sleep) {
        // A power cycle or a normal reboot ends the episode: either the pack was
        // charged and reconnected, or a human is standing there. Start clean.
        clearRtc();
        status_.state = State::Normal;
        APPLOG("guard: armed, trip %umV/cell for %us", GUARD_TRIP_CELL_MV,
                     GUARD_TRIP_SECONDS);
        return false;
    }

    status_.state = State::Recheck;
    status_.sleeps = s.sleeps;
    status_.trip_mv = s.trip_mv;
    recheck_deadline_ = millis() + static_cast<uint32_t>(GUARD_RECHECK_SECONDS) * 1000UL;

    // We are up to read one number, not to serve it. The radio is the single
    // biggest draw on the board and there is no one to talk to anyway.
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();

    APPLOG("guard: woke to re-measure (sleep #%u, was %umV/cell), radio off", s.sleeps,
                 s.trip_mv);
    return true;
#endif
}

bool inRecheck() { return status_.state == State::Recheck; }

void loop(const daly::BmsData& d, bool valid) {
#if GUARD_ENABLE
    if (status_.state == State::Recheck) {
        if (valid) {
            if (!belowRelease(d)) resumeNormal(d);
            sleepNow(d);
        }
        if (static_cast<int32_t>(millis() - recheck_deadline_) >= 0) {
            // No answer from the BMS means no way to tell whether the pack has
            // recovered. Staying awake to keep asking is the one option that can
            // flatten it, so go back to sleep instead and try again later.
            APPLOG("guard: no BMS answer within %us, sleeping again", GUARD_RECHECK_SECONDS);
            sleepNow(d);
        }
        return;
    }

    // Unknown is not low. A silent BMS is the watchdog's problem, not ours --
    // tripping on missing data would turn every comms glitch into an outage.
    if (!valid) {
        low_since_ = 0;
        status_.low_for_s = 0;
        status_.state = State::Normal;
        return;
    }

    // A charger on the pack means the voltage is on its way up. Sagging under a
    // compressor start is not the same thing as a flat battery either, which is
    // what GUARD_TRIP_SECONDS is really for.
    const bool charging = d.pack_current_da > GUARD_CHARGE_HOLDOFF_DA;

    const Thresholds t = thresholds(d);
    status_.limit_cell_mv = t.trip_cell_mv;
    status_.limit_from_bms = t.from_bms;
    status_.near_limit = d.min_cell_mv > 0 && d.min_cell_mv <= t.trip_cell_mv + GUARD_NEAR_MV;

    // Hysteresis: it takes the trip threshold to start counting, but the higher
    // release threshold to stand down, so a pack hovering on the line does not
    // rattle in and out of the countdown.
    const bool low = status_.state == State::Counting ? belowRelease(d) : belowTrip(d);

    if (charging || !low) {
        if (status_.state == State::Counting) {
            APPLOG("guard: stood down at cell %umV%s", d.min_cell_mv,
                         charging ? " (charging)" : "");
        }
        low_since_ = 0;
        status_.low_for_s = 0;
        status_.state = State::Normal;
        return;
    }

    if (status_.state != State::Counting) {
        low_since_ = millis();
        status_.state = State::Counting;
        status_.trip_mv = d.min_cell_mv;
        APPLOG("guard: low, cell %umV (trip %umV, %s) pack %u.%uV, %us to sleep", d.min_cell_mv,
                     t.trip_cell_mv, t.from_bms ? "BMS limit" : "configured",
                     d.pack_voltage_dv / 10, d.pack_voltage_dv % 10, GUARD_TRIP_SECONDS);
    }

    const uint32_t low_ms = millis() - low_since_;
    status_.low_for_s = static_cast<uint16_t>(low_ms / 1000UL);
    if (low_ms >= static_cast<uint32_t>(GUARD_TRIP_SECONDS) * 1000UL) sleepNow(d);
#else
    (void)d;
    (void)valid;
#endif
}

const Status& status() { return status_; }

const char* stateName() {
    switch (status_.state) {
    case State::Disabled: return "disabled";
    case State::Counting: return "counting";
    case State::Recheck: return "recheck";
    default: return "normal";
    }
}

} // namespace voltguard
