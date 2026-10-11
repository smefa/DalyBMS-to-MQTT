// Three power modes, switchable over MQTT. The pure policy is in lib/powercore.
//
//   storage  radio off. Wakes once a day, connects, reports, and sleeps again.
//   normal   radio on, CPU in light sleep between polls, reports every 15 minutes.
//            Anything that talks to the bridge waits up to a second or so.
//   high     radio on in modem sleep, reports every minute.
//
// The mode is kept in flash and survives power loss. Intervals and the other
// knobs are overridable in config.h (POWER_*, see config.h.example).
//
// Storage needs D0 (GPIO16) wired to RST, exactly like the low-voltage guard:
// without it the chip cannot wake itself and stays asleep until power-cycled.
// A power-cycled bridge in storage mode stays up for POWER_STORAGE_GRACE_S after
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

// How long a full storage period is, for expiry of the Home Assistant entities.
uint32_t storageIntervalS();

// Ask for a mode. Safe to call from a network callback: nothing happens until
// the next service().
void request(Mode m);

// Call every loop() pass, after the network has been serviced. Applies a
// requested change and ends a storage session once its report is out; the
// latter does not return. True when a change was applied live (normal <-> high)
// and the caller should re-apply settings().
bool service();

} // namespace powermode
