// Event ring in RTC user memory. Unlike the text log (applog), it survives a watchdog
// or exception reset, so after a hang it still holds the last events before it.
// Not a power cut: RTC memory is cleared then. No flash is written, so nothing wears.
//
// 31 entries of 4 bytes: uptime seconds (16 bits, of the boot that wrote it), event code,
// one byte of detail. Served raw as hex at /events; tools/collect.ps1 decodes it, so
// there are no format strings in the firmware.
#pragma once

#include <Arduino.h>

namespace evlog {

// Keep in step with the decoder in tools/collect.ps1.
enum Code : uint8_t {
    Boot = 1,     // arg: reset reason (rst_info.reason)
    WifiUp = 2,   // arg: -RSSI in dBm
    WifiLost = 3,
    MqttUp = 4,
    MqttDown = 5,
    Radio = 6,    // arg: 1 slow (light/slow modem sleep), 0 normal
    PubFail = 7,  // a state document could not be sent
    Heap = 8,     // arg: free heap / 128
    Block = 9,    // arg: largest free block / 128
    WdogWifi = 10,
    WdogMqtt = 11, // arg: minutes of silence
    WdogBms = 12,
    SleepFailed = 13, // deep sleep did not take; restarted to retry it
};

void add(Code code, uint8_t arg = 0);

// Writes the ring oldest first, one "TTTTCCAA" hex line per entry, and returns the length.
// `cap` should be at least 300.
size_t dumpHex(char* out, size_t cap);

} // namespace evlog
