#include "crashlog.h"

extern "C" {
#include <user_interface.h>
}

namespace crashlog {
namespace {

constexpr uint32_t kMagic = 0x43524C31UL; // "CRL1"
constexpr uint32_t kCapturing = 1;        // a dump is being written, this boot or the last
constexpr uint32_t kSealed = 2;           // begin() has seen it; the next dump starts over
constexpr uint32_t kEmpty = 3;            // no dump since power-on

// Entries below this are stack markers (the core's PRINT_STACK values), the rest
// code addresses.
constexpr uint32_t kCont = 1, kSys = 2, kRom = 4, kBearssl = 8;

// Code addresses are ROM (0x4000xxxx, resolve against eagle.rom.addr.v6.ld), IRAM
// (0x4010xxxx) or flash-mapped (0x402xxxxx); stack and data addresses (0x3fffxxxx)
// in the same dump are left out.
constexpr uint32_t kRomLo = 0x40000000UL, kRomHi = 0x40010000UL;
constexpr uint32_t kIramLo = 0x40100000UL, kIramHi = 0x40110000UL;
constexpr uint32_t kFlashLo = 0x40200000UL, kFlashHi = 0x40300000UL;

// The sys stack comes first in a hardware WDT dump, and with it the frames that
// matter; a deep cont stack would only crowd it out.
constexpr uint16_t kCap = 80;
constexpr uint8_t kWordMax = 10; // "0x" + 8 hex digits

struct Capture {
    uint32_t magic;
    uint32_t state;
    uint32_t reason; // rst_info at seal
    uint32_t epc1;
    uint16_t n;
    uint8_t word_len; // kWordMax + 1 = too long to matter
    bool after_ctx;
    char word[kWordMax];
    uint32_t addr[kCap];
};

// Not zeroed at boot. Valid only while magic, state and n agree; a power-on or a
// flash leaves noise here that fails that check.
Capture cap __attribute__((section(".noinit")));

bool valid() {
    return cap.magic == kMagic &&
           (cap.state == kCapturing || cap.state == kSealed || cap.state == kEmpty) && cap.n <= kCap;
}

// Called from inside the core's crash handlers (and from before the SDK has started),
// so: IRAM, no library calls, no strings in flash.
inline int IRAM_ATTR hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool IRAM_ATTR wordIs(const char* s) {
    uint8_t i = 0;
    for (; s[i]; i++) {
        if (i >= cap.word_len || cap.word[i] != s[i]) return false;
    }
    return i == cap.word_len;
}

void IRAM_ATTR reset() {
    cap.magic = kMagic;
    cap.state = kEmpty;
    cap.n = 0;
    cap.word_len = 0;
    cap.after_ctx = false;
}

inline void IRAM_ATTR push(uint32_t v) {
    if (cap.n < kCap) cap.addr[cap.n++] = v;
}

void IRAM_ATTR endWord() {
    if (cap.word_len == 0 || cap.word_len > kWordMax) return;
    if (cap.after_ctx) {
        cap.after_ctx = false;
        const uint32_t marker = wordIs("cont") ? kCont : wordIs("sys") ? kSys : wordIs("ROM") ? kRom
                              : wordIs("bearssl") ? kBearssl : 0;
        if (marker == 0) return;
        // A dump starts with its first stack. The core prints other lines through here
        // too (a banner on every boot with DEBUG_ESP_HWDT_NOEXTRA4K), and those must
        // not wipe a dump nobody has fetched yet.
        if (cap.state != kCapturing) {
            cap.state = kCapturing;
            cap.n = 0;
        }
        push(marker);
        return;
    }
    if (wordIs("ctx")) {
        cap.after_ctx = true;
        return;
    }
    if (cap.state != kCapturing) return;
    uint8_t i = 0;
    if (cap.word_len == 10 && cap.word[0] == '0' && cap.word[1] == 'x') i = 2;
    else if (cap.word_len != 8) return;
    uint32_t v = 0;
    for (; i < cap.word_len; i++) {
        const int d = hexDigit(cap.word[i]);
        if (d < 0) return;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    if ((v >= kRomLo && v < kRomHi) || (v >= kIramLo && v < kIramHi) || (v >= kFlashLo && v < kFlashHi)) {
        push(v);
    }
}

void IRAM_ATTR take(char c) {
    if (!valid()) reset();
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (alnum) {
        if (cap.word_len < kWordMax) cap.word[cap.word_len] = c;
        if (cap.word_len <= kWordMax) cap.word_len++;
        return;
    }
    endWord();
    cap.word_len = 0;
}

inline void IRAM_ATTR uartPut(char c) {
    while (((USS(0) >> USTXC) & 0xff) >= 0x7e) {
    }
    USF(0) = c;
}

} // namespace

uint16_t begin() {
    if (!valid()) {
        reset();
        return 0;
    }
    // Each boot's printing starts with a clean word.
    cap.word_len = 0;
    cap.after_ctx = false;
    if (cap.state != kCapturing) return 0; // none, or an older dump already reported
    const rst_info* ri = ESP.getResetInfoPtr();
    cap.reason = ri->reason;
    cap.epc1 = ri->epc1;
    cap.state = kSealed;
    return cap.n;
}

uint16_t count() { return valid() && cap.state == kSealed ? cap.n : 0; }

void dump(String& out) {
    if (count() == 0) return;
    char b[20];
    snprintf(b, sizeof b, "reset=%lu", static_cast<unsigned long>(cap.reason));
    out += b;
    snprintf(b, sizeof b, " epc1=%08lx", static_cast<unsigned long>(cap.epc1));
    out += b;
    for (uint16_t i = 0; i < cap.n; i++) {
        const uint32_t v = cap.addr[i];
        if (v < kRomLo) {
            out += v == kCont ? F("\ncont:") : v == kSys ? F("\nsys:") : v == kRom ? F("\nrom:") : F("\nbearssl:");
            continue;
        }
        snprintf(b, sizeof b, " %08lx", static_cast<unsigned long>(v));
        out += b;
    }
    out += '\n';
}

} // namespace crashlog

// Replaces the ROM's character printer (eagle.rom.addr.v6.ld only PROVIDEs it). Only
// the core's crash dumps call it from this image. Same output as the ROM: '\r'
// dropped, '\n' sent as "\r\n".
extern "C" void IRAM_ATTR ets_uart_putc1(char c) {
    crashlog::take(c);
    if (c == '\r') return;
    if (c == '\n') crashlog::uartPut('\r');
    crashlog::uartPut(c);
}
