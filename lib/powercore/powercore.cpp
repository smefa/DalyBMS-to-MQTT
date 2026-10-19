#include "powercore.h"

#include <stdio.h>
#include <string.h>

namespace powercore {
namespace {

constexpr uint32_t kModeMagic = 0x504D4F31UL;  // "PMO1"
constexpr uint32_t kChainMagic = 0x50434831UL; // "PCH1"
constexpr uint32_t kUptimeMagic = 0x50555031UL; // "PUP1"
constexpr uint32_t kOfflineMagic = 0x504F4631UL; // "POF1"
constexpr uint32_t kMask = 0xA5A5A5A5UL;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoreCase(const char* s, size_t len, const char* word) {
    if (strlen(word) != len) return false;
    for (size_t i = 0; i < len; i++) {
        if (lower(s[i]) != word[i]) return false;
    }
    return true;
}

} // namespace

const char* modeName(Mode m) {
    switch (m) {
    case Mode::Storage: return "storage";
    case Mode::High: return "high";
    default: return "normal";
    }
}

bool parseMode(const char* s, size_t len, Mode& out) {
    if (s == nullptr) return false;
    while (len > 0 && isSpace(*s)) {
        s++;
        len--;
    }
    while (len > 0 && isSpace(s[len - 1])) len--;

    if (equalsIgnoreCase(s, len, "storage")) out = Mode::Storage;
    else if (equalsIgnoreCase(s, len, "normal")) out = Mode::Normal;
    else if (equalsIgnoreCase(s, len, "high")) out = Mode::High;
    else return false;
    return true;
}

SleepPlan planSleep(uint32_t remaining_s, uint64_t max_us) {
    // Whole seconds only, so the bookkeeping in `remaining_after_s` is exact.
    uint64_t max_s = max_us / 1000000ULL;
    if (max_s == 0) max_s = 1;

    const uint64_t chunk_s = remaining_s < max_s ? remaining_s : max_s;
    return SleepPlan{chunk_s * 1000000ULL, static_cast<uint32_t>(remaining_s - chunk_s)};
}

ModeRecord makeModeRecord(Mode m) {
    ModeRecord r{kModeMagic, static_cast<uint32_t>(m), 0};
    r.check = r.magic ^ r.mode ^ kMask;
    return r;
}

bool readModeRecord(const ModeRecord& r, Mode& out) {
    if (r.magic != kModeMagic || r.check != (r.magic ^ r.mode ^ kMask)) return false;
    if (r.mode > static_cast<uint32_t>(Mode::High)) return false;
    out = static_cast<Mode>(r.mode);
    return true;
}

ChainRecord makeChainRecord(uint32_t remaining_s) {
    ChainRecord r{kChainMagic, remaining_s, 0};
    r.check = r.magic ^ r.remaining_s ^ kMask;
    return r;
}

UptimeRecord makeUptimeRecord(uint32_t total_s) {
    UptimeRecord r{kUptimeMagic, total_s, 0};
    r.check = r.magic ^ r.total_s ^ kMask;
    return r;
}

bool readUptimeRecord(const UptimeRecord& r, uint32_t& total_s) {
    if (r.magic != kUptimeMagic || r.check != (r.magic ^ r.total_s ^ kMask)) return false;
    total_s = r.total_s;
    return true;
}

OfflineRecord makeOfflineRecord(uint32_t down_s) {
    OfflineRecord r{kOfflineMagic, down_s, 0};
    r.check = r.magic ^ r.down_s ^ kMask;
    return r;
}

bool readOfflineRecord(const OfflineRecord& r, uint32_t& down_s) {
    if (r.magic != kOfflineMagic || r.check != (r.magic ^ r.down_s ^ kMask)) return false;
    down_s = r.down_s;
    return true;
}

bool offlineGiveUp(uint32_t down_s, uint32_t giveup_s) {
    return giveup_s != 0 && down_s >= giveup_s;
}

uint32_t offlineSleepS(uint32_t down_s, uint32_t cycle_s, uint32_t giveup_s, uint32_t offline_s) {
    return offlineGiveUp(down_s, giveup_s) && offline_s > cycle_s ? offline_s : cycle_s;
}

void formatUptime(uint32_t s, char* out, size_t cap) {
    const unsigned long m = (s / 60) % 60, h = (s / 3600) % 24;
    const unsigned long d = (s / 86400) % 365, y = s / (86400UL * 365);
    if (y > 0) snprintf(out, cap, "%lu y %lu d %lu h %lu m", y, d, h, m);
    else if (d > 0) snprintf(out, cap, "%lu d %lu h %lu m", d, h, m);
    else if (h > 0) snprintf(out, cap, "%lu h %lu m", h, m);
    else snprintf(out, cap, "%lu m", m);
}

bool readChainRecord(const ChainRecord& r, uint32_t& remaining_s) {
    if (r.magic != kChainMagic || r.check != (r.magic ^ r.remaining_s ^ kMask)) return false;
    remaining_s = r.remaining_s;
    return true;
}

bool bmsWakeDue(bool bms_answered, uint8_t pulses_sent, uint32_t ms_since_last,
                uint32_t retry_ms, uint8_t max_pulses, uint32_t ms_since_boot,
                uint32_t first_grace_ms) {
    if (pulses_sent == 0) return !bms_answered && ms_since_boot >= first_grace_ms;
    return !bms_answered && pulses_sent < max_pulses && ms_since_last >= retry_ms;
}

} // namespace powercore
