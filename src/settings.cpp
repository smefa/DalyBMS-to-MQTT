#include "settings.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if __has_include("config.h")
#include "config.h"
#endif

#include "applog.h"

// The PIN that unlocks changes. Set SETTINGS_PIN in config.h.
#ifndef SETTINGS_PIN
#error "SETTINGS_PIN must be defined in config.h"
#endif

// Pack voltage profiles, in 0.1 V: normal 14.2-11.0 V, storage 13.2-12.0 V (L2), with L1
// 0.1 V inside. Override in config.h.
#ifndef PACK_NORMAL_HIGH_L1_DV
#define PACK_NORMAL_HIGH_L1_DV 141
#endif
#ifndef PACK_NORMAL_HIGH_L2_DV
#define PACK_NORMAL_HIGH_L2_DV 142
#endif
#ifndef PACK_NORMAL_LOW_L1_DV
#define PACK_NORMAL_LOW_L1_DV 111
#endif
#ifndef PACK_NORMAL_LOW_L2_DV
#define PACK_NORMAL_LOW_L2_DV 110
#endif
#ifndef PACK_STORAGE_HIGH_L1_DV
#define PACK_STORAGE_HIGH_L1_DV 131
#endif
#ifndef PACK_STORAGE_HIGH_L2_DV
#define PACK_STORAGE_HIGH_L2_DV 132
#endif
#ifndef PACK_STORAGE_LOW_L1_DV
#define PACK_STORAGE_LOW_L1_DV 121
#endif
#ifndef PACK_STORAGE_LOW_L2_DV
#define PACK_STORAGE_LOW_L2_DV 120
#endif

namespace settings {
namespace {

constexpr uint8_t kMaxFailures = 5;
constexpr uint32_t kLockoutMs = 60000;

uint8_t failures = 0;
uint32_t locked_at = 0;
bool locked = false;

// Compares every byte whatever the first mismatch, so timing does not leak the PIN.
bool pinMatches(const char* given) {
    const char* want = SETTINGS_PIN;
    const size_t wl = strlen(want);
    const size_t gl = given ? strlen(given) : 0;
    uint8_t diff = wl == gl ? 0 : 1;
    for (size_t i = 0; i < wl; i++) {
        diff |= static_cast<uint8_t>(want[i] ^ (i < gl ? given[i] : 0));
    }
    return diff == 0;
}

Result checkPin(const char* pin) {
    const uint32_t now = millis();
    if (locked && now - locked_at < kLockoutMs) return Result::Locked;
    locked = false;
    if (pinMatches(pin)) {
        failures = 0;
        return Result::Queued;
    }
    if (++failures >= kMaxFailures) {
        locked = true;
        locked_at = now;
        failures = 0;
        APPLOG("settings: %u wrong PINs, locked for %lus", kMaxFailures, kLockoutMs / 1000UL);
    }
    return Result::BadPin;
}

daly::Thresholds merge(const int32_t req[4], uint16_t h1, uint16_t h2, uint16_t l1, uint16_t l2,
                       bool& any) {
    const uint16_t cur[4] = {h1, h2, l1, l2};
    uint16_t v[4];
    for (uint8_t i = 0; i < 4; i++) {
        if (req[i] == kUnset) {
            v[i] = cur[i];
        } else {
            // Out of uint16 range is out of every valid range; clamp so it is rejected,
            // not wrapped into a valid one.
            v[i] = static_cast<uint16_t>(req[i] < 0 ? 0 : (req[i] > 65535 ? 65535 : req[i]));
            any = true;
        }
    }
    return {v[0], v[1], v[2], v[3]};
}

} // namespace

const char* resultText(Result r) {
    switch (r) {
    case Result::Queued: return "queued";
    case Result::BadPin: return "wrong pin";
    case Result::Locked: return "locked, too many wrong pins";
    case Result::NoLimits: return "bms limits not read yet";
    case Result::Invalid: return "values out of range, or a high level not above a low level";
    case Result::Busy: return "busy, a change is still being written";
    default: return "bad request: give at least one limit, and only cell or only pack limits per request";
    }
}

// Clamped first: converting a huge double to an integer is undefined.
int32_t clampRound(double v) {
    if (!(v > -1e6)) return -1000000;
    if (v > 1e6) return 1000000;
    return static_cast<int32_t>(lround(v));
}

bool parseNumber(const char* s, double& out) {
    char* end = nullptr;
    out = strtod(s, &end);
    return end != s && *end == '\0';
}

int32_t packFromVolts(double v) { return clampRound(v * 10.0); }

namespace {

Result applyChecked(DalyBms& bms, const Request& req);

// Order as Request::pack: high L1, high L2, low L1, low L2 (0.1 V).
struct ProfileLevels {
    int32_t v[4];
};

ProfileLevels levels(Profile p) {
    return p == Profile::Storage
               ? ProfileLevels{{PACK_STORAGE_HIGH_L1_DV, PACK_STORAGE_HIGH_L2_DV,
                                PACK_STORAGE_LOW_L1_DV, PACK_STORAGE_LOW_L2_DV}}
               : ProfileLevels{{PACK_NORMAL_HIGH_L1_DV, PACK_NORMAL_HIGH_L2_DV,
                                PACK_NORMAL_LOW_L1_DV, PACK_NORMAL_LOW_L2_DV}};
}

} // namespace

const char* profileName(Profile p) { return p == Profile::Storage ? "storage" : "normal"; }

bool parseProfile(const char* s, size_t len, Profile& out) {
    auto is = [&](const char* name) { return strlen(name) == len && strncasecmp(s, name, len) == 0; };
    if (is("normal")) out = Profile::Normal;
    else if (is("storage")) out = Profile::Storage;
    else return false;
    return true;
}

const char* currentProfile(const daly::BmsData& d) {
    if (d.pack_low_l1_dv == 0) return "unknown";
    for (Profile p : {Profile::Normal, Profile::Storage}) {
        const ProfileLevels l = levels(p);
        if (d.pack_high_l1_dv == l.v[0] && d.pack_high_l2_dv == l.v[1] &&
            d.pack_low_l1_dv == l.v[2] && d.pack_low_l2_dv == l.v[3]) {
            return profileName(p);
        }
    }
    return "custom";
}

int32_t profileHighDv(Profile p) { return levels(p).v[1]; }
int32_t profileLowDv(Profile p) { return levels(p).v[3]; }

namespace {

Request profileRequest(Profile p) {
    const ProfileLevels l = levels(p);
    Request req;
    for (uint8_t i = 0; i < 4; i++) req.pack[i] = l.v[i];
    return req;
}

} // namespace

Result applyProfile(DalyBms& bms, const char* pin, Profile p) {
    const Result pr = checkPin(pin);
    if (pr != Result::Queued) return pr;
    return applyChecked(bms, profileRequest(p));
}

Result applyProfileTrusted(DalyBms& bms, Profile p) { return applyChecked(bms, profileRequest(p)); }

Result apply(DalyBms& bms, const char* pin, const Request& req) {
    const Result pr = checkPin(pin);
    if (pr != Result::Queued) return pr;
    return applyChecked(bms, req);
}

namespace {

Result applyChecked(DalyBms& bms, const Request& req) {
    const daly::BmsData& d = bms.data();
    if (bms.writePending()) return Result::Busy;

    bool cell_any = false, pack_any = false;
    const daly::Thresholds cell = merge(req.cell, d.cell_high_l1_mv, d.cell_high_l2_mv,
                                        d.cell_low_l1_mv, d.cell_low_l2_mv, cell_any);
    const daly::Thresholds pack = merge(req.pack, d.pack_high_l1_dv, d.pack_high_l2_dv,
                                        d.pack_low_l1_dv, d.pack_low_l2_dv, pack_any);
    const bool sleep_any = req.sleep_s != kUnset;
    if (!cell_any && !pack_any && !sleep_any) return Result::BadRequest;
    // Only one write can wait at a time, so one group per request.
    if ((cell_any ? 1 : 0) + (pack_any ? 1 : 0) + (sleep_any ? 1 : 0) > 1) {
        return Result::BadRequest;
    }
    if (sleep_any) {
        if (!d.have_sleep) return Result::NoLimits;
        if (req.sleep_s < daly::SLEEP_TIMEOUT_MIN_S || req.sleep_s > 65535) return Result::Invalid;
        if (!bms.queueSleepTimeout(static_cast<uint16_t>(req.sleep_s))) return Result::Busy;
        APPLOG("settings: sleep timeout change accepted");
        return Result::Queued;
    }
    // Only the group being changed has to be known: the rest of it is merged in.
    if (cell_any && d.cell_low_l1_mv == 0) return Result::NoLimits;
    if (pack_any && d.pack_low_l1_dv == 0) return Result::NoLimits;
    if (cell_any && !daly::validCellThresholds(cell)) return Result::Invalid;
    if (pack_any && !daly::validPackThresholds(pack)) return Result::Invalid;

    const bool ok = cell_any ? bms.queueCellLimits(cell) : bms.queuePackLimits(pack);
    if (!ok) return Result::Busy;
    APPLOG("settings: %s limits change accepted", cell_any ? "cell" : "pack");
    return Result::Queued;
}

} // namespace

Result applyJson(DalyBms& bms, const char* json, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) return Result::BadRequest;

    static const char* const kCell[4] = {"cell_high_l1_mv", "cell_high_l2_mv", "cell_low_l1_mv",
                                         "cell_low_l2_mv"};
    static const char* const kPack[4] = {"pack_high_l1_v", "pack_high_l2_v", "pack_low_l1_v",
                                         "pack_low_l2_v"};
    // {"pin":"...","profile":"storage"} sets all four pack levels from config.h.
    if (!doc["profile"].isNull()) {
        const char* name = doc["profile"].is<const char*>() ? doc["profile"].as<const char*>() : nullptr;
        Profile p;
        if (!name || !parseProfile(name, strlen(name), p)) return Result::BadRequest;
        // A profile is a whole request; other fields beside it would be ignored.
        for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
            const char* k = kv.key().c_str();
            if (strcmp(k, "pin") != 0 && strcmp(k, "profile") != 0) return Result::BadRequest;
        }
        return applyProfile(bms, doc["pin"] | "", p);
    }
    Request req;
    // A field that is present but not a number is an error, not "leave as is".
    for (uint8_t i = 0; i < 4; i++) {
        if ((doc[kCell[i]].is<JsonVariantConst>() && !doc[kCell[i]].isNull() && !doc[kCell[i]].is<double>()) ||
            (!doc[kPack[i]].isNull() && !doc[kPack[i]].is<double>())) {
            return Result::BadRequest;
        }
    }
    if (!doc["sleep_timeout_s"].isNull() && !doc["sleep_timeout_s"].is<double>()) {
        return Result::BadRequest;
    }
    for (uint8_t i = 0; i < 4; i++) {
        if (doc[kCell[i]].is<double>()) req.cell[i] = clampRound(doc[kCell[i]].as<double>());
        if (doc[kPack[i]].is<double>()) req.pack[i] = packFromVolts(doc[kPack[i]].as<double>());
    }
    if (doc["sleep_timeout_s"].is<double>()) req.sleep_s = clampRound(doc["sleep_timeout_s"].as<double>());
    return apply(bms, doc["pin"] | "", req);
}

} // namespace settings
