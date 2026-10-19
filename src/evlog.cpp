#include "evlog.h"

namespace evlog {
namespace {

// Offsets are in 4-byte blocks. The guard and the power mode use 64..95 (see
// voltguard.cpp, powermode.cpp); this takes the rest of the 128-block user area.
constexpr uint32_t kBase = 96;
constexpr uint8_t kSlots = 31; // block kBase is the header, 97..127 the entries
constexpr uint32_t kMagic = 0xE7A1;

struct Header {
    uint8_t head;  // where the next entry goes
    uint8_t count; // valid entries, up to kSlots
};

// Header word: magic << 16 | head << 8 | count. Anything else (power-up garbage)
// starts the ring empty.
Header readHeader() {
    uint32_t w = 0;
    if (!ESP.rtcUserMemoryRead(kBase, &w, sizeof w) || (w >> 16) != kMagic) return {0, 0};
    const Header h{static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w)};
    if (h.head >= kSlots || h.count > kSlots) return {0, 0};
    return h;
}

void writeHeader(const Header& h) {
    uint32_t w = (kMagic << 16) | (static_cast<uint32_t>(h.head) << 8) | h.count;
    ESP.rtcUserMemoryWrite(kBase, &w, sizeof w);
}

} // namespace

void add(Code code, uint8_t arg) {
    Header h = readHeader();
    const uint32_t t = (millis() / 1000UL) & 0xFFFF;
    uint32_t w = (t << 16) | (static_cast<uint32_t>(code) << 8) | arg;
    ESP.rtcUserMemoryWrite(kBase + 1 + h.head, &w, sizeof w);
    h.head = static_cast<uint8_t>((h.head + 1) % kSlots);
    if (h.count < kSlots) h.count++;
    writeHeader(h);
}

size_t dumpHex(char* out, size_t cap) {
    const Header h = readHeader();
    const uint8_t start = h.count == kSlots ? h.head : 0;
    size_t n = 0;
    for (uint8_t i = 0; i < h.count && n + 9 < cap; i++) {
        uint32_t w = 0;
        ESP.rtcUserMemoryRead(kBase + 1 + (start + i) % kSlots, &w, sizeof w);
        n += snprintf(out + n, cap - n, "%08lx\n", static_cast<unsigned long>(w));
    }
    if (n < cap) out[n] = '\0';
    return n;
}

} // namespace evlog
