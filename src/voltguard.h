// Low-voltage guard: parks the D1 Mini in deep sleep before its own ~20 mA can
// drag the pack below the point where the BMS gives up.
//
// This exists because of a wiring decision. The buck converter feeding the
// bridge sits *ahead* of the BMS output MOSFETs, so telemetry survives a
// discharge cutoff -- which is exactly when you want it. The cost is that the
// BMS's own low-voltage protection no longer protects the pack from us: after
// cutoff the bridge is the only load left, and it will happily keep draining a
// LiFePO4 pack into the ground over a week. Nothing but this file stops it.
//
// So: watch the numbers the BMS already gives us, and when the pack is genuinely
// low, sleep. Wake occasionally, take one measurement with the radio off, and go
// back to sleep until the pack has actually recovered.
//
// REQUIRES D0 (GPIO16) WIRED TO RST. Without that jumper the ESP8266 cannot wake
// itself and the first trip is permanent until you power-cycle it by hand.
// See docs/wiring.md.
#pragma once

#include <Arduino.h>

#include "daly_protocol.h"

namespace voltguard {

enum class State : uint8_t {
    Disabled, // GUARD_ENABLE is 0
    Normal,   // above the release thresholds
    Counting, // below trip, waiting out GUARD_TRIP_SECONDS before acting
    Recheck,  // awake after a guard sleep, deciding whether to sleep again
};

struct Status {
    State state = State::Normal;
    uint16_t low_for_s = 0; // how long we have been counting
    uint16_t sleeps = 0;    // consecutive guard sleeps in this episode
    uint16_t trip_mv = 0;   // lowest cell that sent us to sleep, 0 if never
    uint16_t limit_cell_mv = 0; // cell voltage the guard currently trips at
    bool limit_from_bms = false; // true when that came from the BMS's own settings
    bool near_limit = false;     // close enough to the trip that readings should speed up
};

// Restores the guard state saved across deep sleep. Call this first in setup(),
// before WiFi: a true return means we woke only to re-measure, and the caller
// must skip WiFi, OTA, MQTT and HTTP entirely.
bool begin();

// True while we are awake solely to take one measurement.
bool inRecheck();

// Feed once per loop, after bms.loop(). May not return -- going to sleep is how
// this function succeeds.
void loop(const daly::BmsData& d, bool valid);

const Status& status();
const char* stateName();

} // namespace voltguard
