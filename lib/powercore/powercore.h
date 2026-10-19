// Power-mode policy with no Arduino dependency, so it unit-tests on the host.
//
//   Storage  radio off, wakes once a day to report
//   Normal   radio on at its slowest, reports every 15 minutes
//   High     radio on, reports every minute
//
// The Arduino side (src/powermode.cpp) owns the hardware; this file owns the
// decisions that are easy to get wrong and expensive to find out about later:
// parsing the command, planning a sleep that is longer than the chip can do in
// one go, and recognising uninitialised flash/RTC memory.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace powercore {

enum class Mode : uint8_t { Storage = 0, Normal = 1, High = 2 };

const char* modeName(Mode m);

// Accepts the names above, any case, surrounding whitespace ignored. `s` need
// not be NUL-terminated (it is an MQTT payload). False leaves `out` untouched.
bool parseMode(const char* s, size_t len, Mode& out);

// --- long sleeps ------------------------------------------------------------
//
// ESP.deepSleep() tops out around an hour, far short of a day. A long sleep is
// a chain: sleep one chunk, wake with the radio untouched, sleep the next, and
// report only after the last. `remaining_after_s` is what is still owed once
// this chunk is over; it is what gets parked in RTC memory across the sleep.
struct SleepPlan {
    uint64_t sleep_us;
    uint32_t remaining_after_s;
};

// `max_us` is the longest single sleep the hardware allows. Always makes
// progress: never returns a zero-length sleep for a non-zero request.
SleepPlan planSleep(uint32_t remaining_s, uint64_t max_us);

// --- records that must survive a reset --------------------------------------
//
// Both the persisted mode (flash) and the sleep chain (RTC memory) are guarded
// by a magic number and a check word. This is not a CRC: it only has to catch
// memory nobody ever wrote, which is the one failure that actually happens.

struct ModeRecord {
    uint32_t magic;
    uint32_t mode; // widened so the struct has no padding to checksum
    uint32_t check;
};

ModeRecord makeModeRecord(Mode m);
bool readModeRecord(const ModeRecord& r, Mode& out);

struct ChainRecord {
    uint32_t magic;
    uint32_t remaining_s;
    uint32_t check;
};

ChainRecord makeChainRecord(uint32_t remaining_s);
bool readChainRecord(const ChainRecord& r, uint32_t& remaining_s);

// --- uptime ------------------------------------------------------------------
//
// Deep sleep reboots the chip, so millis() alone restarts every wake. The running
// total is parked in RTC memory across each sleep instead.

struct UptimeRecord {
    uint32_t magic;
    uint32_t total_s;
    uint32_t check;
};

UptimeRecord makeUptimeRecord(uint32_t total_s);
bool readUptimeRecord(const UptimeRecord& r, uint32_t& total_s);

// --- no WiFi ------------------------------------------------------------------
//
// A bridge stored somewhere with no network would otherwise keep hunting for one
// from the pack it is supposed to be minding. The time without WiFi is totalled in
// RTC memory across sleeps and restarts; once it passes `giveup_s`, the bridge
// tests for a network only once per `offline_s` instead of on its normal cycle.

struct OfflineRecord {
    uint32_t magic;
    uint32_t down_s; // time without WiFi, sleeping included
    uint32_t check;
};

OfflineRecord makeOfflineRecord(uint32_t down_s);
bool readOfflineRecord(const OfflineRecord& r, uint32_t& down_s);

// True once the time without WiFi has reached the point of giving up. A `giveup_s`
// of 0 turns the rule off: never.
bool offlineGiveUp(uint32_t down_s, uint32_t giveup_s);

// How long to sleep after a session that found no network: the mode's own
// `cycle_s`, or `offline_s` once giving up (never shortened to below the cycle).
uint32_t offlineSleepS(uint32_t down_s, uint32_t cycle_s, uint32_t giveup_s, uint32_t offline_s);

// "45 m", "3 h 20 m", "2 d 4 h 24 m", "1 y 23 d 4 h 24 m": units that are zero at
// the front are left out, and a year is 365 days. `cap` of 24 holds any value.
void formatUptime(uint32_t total_s, char* out, size_t cap);

// --- waking the BMS ---------------------------------------------------------
//
// A storage bridge comes up after a long sleep, and by then the BMS may have gone
// to sleep too. Shorting its S1 pad to ground wakes it (src/bmswake.cpp). Nothing
// is pulsed while the BMS answers. If it has not answered `first_grace_ms` after
// boot the first pulse goes out; after that, one more every `retry_ms` for as long
// as it stays silent, up to `max_pulses` in all.
bool bmsWakeDue(bool bms_answered, uint8_t pulses_sent, uint32_t ms_since_last,
                uint32_t retry_ms, uint8_t max_pulses, uint32_t ms_since_boot,
                uint32_t first_grace_ms);

} // namespace powercore
