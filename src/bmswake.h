// Wakes a sleeping BMS by shorting its S1 pad to ground (see docs/wiring.md).
//
// Storage mode sleeps for a day at a time, and the BMS has usually gone to sleep
// by the time the bridge is back. Without a wake the first sweeps of the report
// time out and the report goes out with no readings.
//
// Opt-in: define BMS_WAKE_PIN in config.h. Without it every call here does nothing.
// The pin is an input (high-Z) except during a pulse, because S1 idles at about
// 2.9 V on the BMS's own pull-up and must never be driven high.
#pragma once

#include <stdint.h>

namespace bmswake {

// Call once in setup(), before anything may pulse.
void begin();

// One wake pulse, now. Blocks for the pulse (BMS_WAKE_PULSE_MS) plus a short
// settle time for the BMS to come up. Does nothing without BMS_WAKE_PIN.
void pulse();

// From loop(), in every power mode: pulses only if `bms_answered` is still
// false BMS_WAKE_GRACE_S after boot, then again every BMS_WAKE_RETRY_S while it
// stays false, BMS_WAKE_MAX_PULSES in all. An awake BMS is never pulsed.
// True if it pulsed, so the caller can ask the BMS something while it is awake.
bool serviceStorage(bool bms_answered);

} // namespace bmswake
