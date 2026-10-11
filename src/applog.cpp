#include "applog.h"

#include <stdarg.h>
#include <stdio.h>

namespace applog {
namespace {

char buffer[LINES][LINE_LEN];
uint8_t next = 0;   // where the next line goes
bool wrapped = false;

} // namespace

namespace {

// Writes the uptime prefix for the next line and returns where the message goes.
char* beginLine(int& n) {
    char* line = buffer[next];

    // Prefix every line with uptime so the ordering is obvious in /log.
    const uint32_t s = millis() / 1000;
    n = snprintf(line, LINE_LEN, "[%5lu.%03lu] ",
                     static_cast<unsigned long>(s),
                     static_cast<unsigned long>(millis() % 1000));
    // snprintf returns what it *would* have written, so clamp before using it
    // as an offset -- otherwise a truncated prefix yields a negative remaining
    // length that converts to a huge size_t.
    if (n < 0) n = 0;
    if (n > LINE_LEN - 1) n = LINE_LEN - 1;
    return line;
}

void endLine() {
    next = static_cast<uint8_t>((next + 1) % LINES);
    if (next == 0) wrapped = true;
}

} // namespace

void logf(const char* fmt, ...) {
    int n;
    char* line = beginLine(n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, LINE_LEN - n, fmt, ap);
    va_end(ap);
    endLine();
}

void logf_P(PGM_P fmt, ...) {
    int n;
    char* line = beginLine(n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_P(line + n, LINE_LEN - n, fmt, ap);
    va_end(ap);
    endLine();
}

void dump(String& out) {
    const uint8_t start = wrapped ? next : 0;
    const uint8_t count = wrapped ? LINES : next;
    for (uint8_t i = 0; i < count; i++) {
        out += buffer[(start + i) % LINES];
        out += '\n';
    }
}

} // namespace applog
