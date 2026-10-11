// In-RAM ring buffer of log lines, served at http://<esp-ip>/log.
//
// UART0 TX (GPIO1) is wired to the BMS, so printing to Serial would inject garbage into
// the DALY bus. Nothing in this firmware may write to Serial.
#pragma once

#include <Arduino.h>

namespace applog {

constexpr uint8_t LINES = 32;
constexpr uint8_t LINE_LEN = 80;

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Same, but `fmt` lives in flash. String literals cost RAM on the ESP8266 -- about
// 2 kB across this firmware's log lines -- and with TLS up there is only a few
// kB of heap to spare. Use APPLOG() rather than calling this directly.
void logf_P(PGM_P fmt, ...);
#define APPLOG(fmt, ...) ::applog::logf_P(PSTR(fmt), ##__VA_ARGS__)

// Appends the whole buffer, oldest first, to `out`.
void dump(String& out);

} // namespace applog
