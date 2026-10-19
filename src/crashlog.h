// Keeps the code addresses from the core's crash dumps across the reset, for /crash.
//
// The core prints a stack dump on UART0 after an exception or soft WDT (postmortem)
// and, built with DEBUG_ESP_HWDT_NOEXTRA4K, at the boot after a hardware WDT. UART0
// TX is the BMS request line, so nobody is listening. Both dumps print through
// ets_uart_putc1(), which the ROM provides only as a linker PROVIDE; crashlog.cpp
// defines it, still prints, and keeps every word that looks like a code address in
// RAM that the reset does not clear (.noinit). Resolve them against the build's
// firmware.elf; innermost frames come first in each stack.
#pragma once

#include <Arduino.h>

namespace crashlog {

// Seals a dump written before this boot, so a later one starts afresh, and returns
// how many entries it kept (0: the last reset left none). Call early in setup(): a
// hardware WDT dump is written before setup() runs, a postmortem one just before
// the reset.
uint16_t begin();

// Entries in the kept dump, which stays until the next one (or a power cut); 0 if none.
uint16_t count();

// The kept dump as text: the reset reason and epc1 of the boot it ended, then one
// line per stack ("exc", "sys", "cont", "bearssl"), the addresses as hex. Empty if
// there is none.
void dump(String& out);

} // namespace crashlog
