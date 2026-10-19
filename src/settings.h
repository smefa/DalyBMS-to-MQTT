// Changing the BMS's voltage limits from the web page or over MQTT.
//
// Both entry points go through here so they share one PIN check, one lockout and
// one set of validation rules. Quantities use the names of status.json's "limits":
// cell_*_mv in millivolts, pack_*_v in volts.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "daly_bms.h"

namespace settings {

enum class Result : uint8_t {
    Queued,     // accepted; the BMS is written between sweeps
    BadPin,
    Locked,     // too many wrong PINs recently
    NoLimits,   // the BMS's current limits are not known yet, so nothing to merge with
    Invalid,    // out of range or a high level not above a low level
    Busy,       // an earlier change is still being written
    BadRequest, // not parseable, or no limit given
};

const char* resultText(Result r);

// A field left at kUnset keeps the BMS's current value. Packs are in 0.1 V.
constexpr int32_t kUnset = INT32_MIN; // outside anything a request can parse to
struct Request {
    int32_t cell[4] = {kUnset, kUnset, kUnset, kUnset}; // high L1, high L2, low L1, low L2 (mV)
    int32_t pack[4] = {kUnset, kUnset, kUnset, kUnset}; // same order (0.1 V)
    int32_t sleep_s = kUnset;                           // BMS sleep timeout (s)
};

// Two ready-made sets of pack limits (levels in config.h, applied to L1 and L2 alike).
enum class Profile : uint8_t { Normal, Storage };
const char* profileName(Profile p);
bool parseProfile(const char* s, size_t len, Profile& out);
int32_t profileHighDv(Profile p); // 0.1 V
int32_t profileLowDv(Profile p);
// "normal", "storage" when the BMS's pack limits match one, else "custom" / "unknown".
const char* currentProfile(const daly::BmsData& d);
Result applyProfile(DalyBms& bms, const char* pin, Profile p);
// No PIN: for the MQTT select, where the broker login is the gate (as for the power mode).
Result applyProfileTrusted(DalyBms& bms, Profile p);

// Checks the PIN, merges `req` with the BMS's current limits, validates and queues.
Result apply(DalyBms& bms, const char* pin, const Request& req);

// Same from a JSON document {"pin":"...","cell_high_l1_mv":3650,"pack_low_l1_v":11.8,...}.
Result applyJson(DalyBms& bms, const char* json, size_t len);

// Rounds to a whole number, clamped to +-1e6 first (NaN counts as the low end), so
// a hostile or garbled number can never reach an undefined float-to-int cast.
int32_t clampRound(double v);

// Parses one form/JSON value in volts (pack) or millivolts (cell) into Request units.
int32_t packFromVolts(double v);

// Whole-string number; false for "11,9" or "12abc", which toFloat() would half-read.
bool parseNumber(const char* s, double& out);

} // namespace settings
