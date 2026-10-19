#include "bmswake.h"

#include <Arduino.h>

#if __has_include("config.h")
#include "config.h"
#else
#error "src/config.h is missing. Copy src/config.h.example to src/config.h and fill it in."
#endif

#include "applog.h"
#include "powercore.h"

#ifndef BMS_WAKE_PULSE_MS
#define BMS_WAKE_PULSE_MS 300
#endif
#ifndef BMS_WAKE_SETTLE_MS
#define BMS_WAKE_SETTLE_MS 500 // let the BMS come up before the first request
#endif
#ifndef BMS_WAKE_GRACE_S
#define BMS_WAKE_GRACE_S 3 // silence at boot this long before the first pulse
#endif
#ifndef BMS_WAKE_RETRY_S
#define BMS_WAKE_RETRY_S 10
#endif
#ifndef BMS_WAKE_MAX_PULSES
#define BMS_WAKE_MAX_PULSES 3
#endif
#ifndef BMS_WAKE_REARM_S
#define BMS_WAKE_REARM_S 300 // a BMS silent this long after the last pulse gets a fresh set
#endif

namespace bmswake {
namespace {

uint8_t pulses_ = 0;
uint32_t last_pulse_ms_ = 0;

} // namespace

#ifdef BMS_WAKE_PIN

void begin() {
    pinMode(BMS_WAKE_PIN, INPUT); // high-Z until a pulse
}

void pulse() {
    pinMode(BMS_WAKE_PIN, OUTPUT);
    digitalWrite(BMS_WAKE_PIN, LOW);
    delay(BMS_WAKE_PULSE_MS);
    pinMode(BMS_WAKE_PIN, INPUT);
    delay(BMS_WAKE_SETTLE_MS);

    pulses_++;
    last_pulse_ms_ = millis();
    APPLOG("bms: wake pulse %u", static_cast<unsigned>(pulses_));
}

bool serviceStorage(bool bms_answered) {
    // The pulse budget is per silence, not per boot: a BMS that answered and later
    // dozed off must be woken again, even when its pulses were spent earlier. One that
    // never answers gets a fresh set every REARM_S.
    if (pulses_ > 0 && (bms_answered ||
                        (pulses_ >= BMS_WAKE_MAX_PULSES &&
                         millis() - last_pulse_ms_ >= static_cast<uint32_t>(BMS_WAKE_REARM_S) * 1000UL))) {
        pulses_ = 0;
    }
    // A fresh budget must not skip the cooldown after the last pulse.
    if (pulses_ == 0 && last_pulse_ms_ != 0 &&
        millis() - last_pulse_ms_ < static_cast<uint32_t>(BMS_WAKE_RETRY_S) * 1000UL) {
        return false;
    }
    if (powercore::bmsWakeDue(bms_answered, pulses_, millis() - last_pulse_ms_,
                              static_cast<uint32_t>(BMS_WAKE_RETRY_S) * 1000UL,
                              BMS_WAKE_MAX_PULSES, millis(),
                              static_cast<uint32_t>(BMS_WAKE_GRACE_S) * 1000UL)) {
        pulse();
        return true;
    }
    return false;
}

#else

void begin() {}
void pulse() {}
bool serviceStorage(bool) { return false; }

#endif

} // namespace bmswake
