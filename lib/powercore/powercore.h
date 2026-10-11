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

} // namespace powercore
