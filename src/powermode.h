// Three power modes, switchable over MQTT. The pure policy is in lib/powercore.
//
//   storage  deep sleep. Wakes once a day, connects, reports, and sleeps again.
//   normal   the same cycle every 15 minutes. A mode command is picked up at the
//            next wake, so a switch to high can take up to that long.
//   high     radio on, CPU in light sleep between polls, reports every 2 minutes. The only mode that is
//            online all the time.
//
// The mode is kept in flash and survives power loss. Intervals and the other
// knobs are overridable in config.h (POWER_*, see config.h.example).
//
// Normal and storage need D0 (GPIO16) wired to RST, exactly like the low-voltage
// guard: without it the chip cannot wake itself and stays asleep until power-cycled.
// High needs it too once WiFi has been missing for POWER_OFFLINE_GIVEUP_S (it then
// sleeps like the others); set that to 0 on a board without the jumper.
// A power-cycled bridge in either mode stays up for POWER_STORAGE_GRACE_S after
// it has reported, so there is a window to switch it back (or to flash it).
#pragma once

#include <stdint.h>

#include "powercore.h"

namespace powermode {

using Mode = powercore::Mode;

struct Settings {
    uint32_t publish_ms;   // state publish interval
    uint32_t bms_poll_ms;  // time between BMS sweeps
    uint32_t bms_stale_ms; // BMS data older than this is stale
    uint32_t ping_ms;      // MQTT keepalive ping; keeps proxies from dropping the socket
    bool light_sleep;      // CPU sleeps between radio events
    uint8_t listen_interval; // beacons between wakeups; 0 = every DTIM
};

// Call first in setup(), before anything else touches the hardware. When this is
// a wake in the middle of a storage sleep it goes straight back to sleep and
// does not return.
void sleepIfMidChain();

// Loads the persisted mode. Call after sleepIfMidChain().
void begin();

Mode mode();
const char* modeName();
const Settings& settings();

// Seconds since power-up, counting the time spent in deep sleep (the planned sleep
// length scaled by POWER_SLEEP_CLOCK_PERMILLE, so still a percent or two off). It
// carries across a deep sleep and across the restart of a mode change; a power-up,
// crash or OTA restart starts it again. Unlike millis() it does not wrap at 49 days.
uint32_t uptimeS();

// True in the modes that deep-sleep between reports (normal and storage).
bool sleeps();

// The period between reports of the current sleeping mode, for expiry of the Home
// Assistant entities. High reports the storage period, the longest one.
uint32_t sleepIntervalS();

// Tell it whether WiFi is connected; call every loop() pass. It totals the time
// without WiFi in RTC memory, across sleeps and restarts, for the give-up rule in
// service(): after POWER_OFFLINE_GIVEUP_S without WiFi, every mode tests for a
// network only once per POWER_OFFLINE_INTERVAL_S. A connection clears the total.
void noteWifi(bool up);

// Writes the no-WiFi total to RTC memory, for a restart the caller is about to make.
void saveOffline();

// Ask for a mode. Safe to call from a network callback: nothing happens until
// the next service().
void request(Mode m);

// Call every loop() pass, after the network has been serviced. Applies a
// requested change (which restarts into the new mode) and ends a sleeping-mode
// session once its report is out; neither returns. Always false otherwise; the
// value is kept for callers that re-apply settings() after a live change.
bool service();

} // namespace powermode
