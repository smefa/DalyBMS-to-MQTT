// DALY BMS -> ESP8266 (Wemos D1 Mini) -> Home Assistant (MQTT over WebSocket)
//
// The BMS is split across two UARTs:
//   - requests go out on hardware UART0 TX (GPIO1), shared with the USB bridge's
//     RX input, which does not drive the line;
//   - replies come in on a receive-only SoftwareSerial on D5 (GPIO14).
// GPIO3 (UART0 RX) cannot take the replies: the CH340's TX output is wired
// straight to it with no series resistor and overpowers the BMS's weak driver.
//
// Consequence, load-bearing: NOTHING may print to Serial, because GPIO1 is the
// BMS request line. Use APPLOG() and read http://<ip>/log.

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
extern "C" {
#include <user_interface.h>
}

#if __has_include("config.h")
#include "config.h"
#else
#error "src/config.h is missing. Copy src/config.h.example to src/config.h and fill it in."
#endif

#include "applog.h"
#include "daly_bms.h"
#include "ha_mqtt.h"
#include "powermode.h"
#include "voltguard.h"
#include "webserver.h"

namespace {

DalyBms bms;

constexpr uint8_t kBmsRxPin = 14; // D5 <- BMS TX (white)
// 128 bytes holds the largest reply (0x95 on 4S, two 13-byte frames) many times over.
constexpr int kBmsRxBuffer = 128;

SoftwareSerial bms_rx;

// Reads from the receive-only SoftwareSerial, writes to hardware UART0 TX.
class BmsPort : public Stream {
public:
    int available() override { return bms_rx.available(); }
    int read() override { return bms_rx.read(); }
    int peek() override { return bms_rx.peek(); }
    size_t write(uint8_t b) override { return Serial.write(b); }
    size_t write(const uint8_t* buf, size_t n) override { return Serial.write(buf, n); }
    void flush() override { Serial.flush(); }
};
BmsPort bms_port;

uint32_t wifi_down_since = 0;
uint32_t last_wifi_attempt = 0;
bool wifi_was_up = false;
bool ota_in_progress = false;

constexpr uint32_t kWifiRetryMs = 15000;

// Longest single nap in normal mode. Incoming network traffic cuts it short, so
// this only bounds how late the loop notices things that make no packet
// (a WiFi drop, a timer).
constexpr uint32_t kIdleSliceMs = 1000;

// Networks in the order they are tried. The external one comes first: when it
// is in range we are away from home and the internal one cannot be. Each also
// decides which MQTT server to use, since the internal hostname is not
// reachable from outside and vice versa.
struct WifiProfile {
    const char* ssid;
    const char* password;
    bool external;
};
constexpr WifiProfile kProfiles[] = {
#ifdef WIFI_SSID_EXT
    {WIFI_SSID_EXT, WIFI_PASSWORD_EXT, true},
#endif
    {WIFI_SSID, WIFI_PASSWORD, false},
};
constexpr uint8_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);
uint8_t profile_idx = 0;

void beginProfile(uint8_t idx) {
    profile_idx = idx;
    WiFi.begin(kProfiles[idx].ssid, kProfiles[idx].password);
    last_wifi_attempt = millis();
    APPLOG("wifi: connecting to %s (%s)", kProfiles[idx].ssid,
                 kProfiles[idx].external ? "external" : "internal");
}

// How the radio sleeps, per power mode (src/powermode.h).
//
// High: modem sleep. The radio parks between the AP's DTIM beacons and wakes to
// collect whatever was buffered for it. Roughly 75 mA -> 20 mA average, which
// matters because this board runs off the pack it is measuring. The cost is
// latency on the first packet of an exchange -- one DTIM period, so typically
// 100-300 ms, set by the access point rather than by us.
//
// Normal: light sleep (POWER_NORMAL_LIGHT_SLEEP). The radio still parks between
// DTIM beacons, and the CPU sleeps too while loop() is parked in delay() -- see
// the end of loop(). The CPU is most of what an idle bridge draws, so this is
// where normal mode gets its saving.
//
// The listen interval (POWER_NORMAL_LISTEN_INTERVAL) would stretch the radio's
// wakeups past DTIM, but the core only passes it to the SDK on NONOS SDK 3.x.
// This build uses the default SDK 2.2.x, where it is ignored.
//
// The slow radio waits until the MQTT session has settled, for two reasons.
// The TLS handshake is one long blocking call that waits on the network many
// times; under a slow radio it took 40 s instead of 4 on the bench, long enough
// for the software watchdog to bite. And switching to light sleep in the middle
// of the burst after a connect (discovery, first state) crashed the board with
// a Soft WDT reset in roughly one boot in five to ten, as the first BMS sweep
// under light sleep landed among those publishes. So connect and catch up at
// full attention, slow down once things are quiet, and speed up again if the
// link drops.
bool radio_slow = false;
bool mqtt_settled = false;
uint32_t settled_at = 0; // when mqtt_settled last became true

constexpr uint32_t kRadioSettleMs = 10000;

void trackSettled() {
    const bool now = ha_mqtt::settled();
    if (now && !mqtt_settled) settled_at = millis();
    mqtt_settled = now;
}

bool wantSlowRadio() {
    const powermode::Settings& s = powermode::settings();
    return (s.light_sleep || s.listen_interval > 0) && mqtt_settled &&
           millis() - settled_at >= kRadioSettleMs;
}

void applyWifiSleep() {
    const powermode::Settings& s = powermode::settings();
    const bool slow = wantSlowRadio();
    WiFi.setSleepMode(slow && s.light_sleep ? WIFI_LIGHT_SLEEP : WIFI_MODEM_SLEEP,
                      slow ? s.listen_interval : 0);
    if (slow != radio_slow) {
        APPLOG("power: radio %s", slow ? (s.light_sleep ? "light sleep" : "slow modem sleep")
                                             : "normal modem sleep");
    }
    radio_slow = slow;
}

// The guard decides on GUARD_TRIP_SECONDS of readings. A slow normal-mode poll
// would give it only one or two, and a sag that has already passed would then
// trip it before the next reading could clear it. So while it is counting, poll
// at the fast rate.
void applyPolling() {
    const powermode::Settings& s = powermode::settings();
    const voltguard::Status& g = voltguard::status();
    const bool urgent = g.state == voltguard::State::Counting || g.near_limit;
    const uint32_t fast = POLL_INTERVAL_MS;
    bms.setPolling(urgent && fast < s.bms_poll_ms ? fast : s.bms_poll_ms, s.bms_stale_ms);
}

// Everything a mode changes that is not the radio's connection itself.
void applyMode() {
    const powermode::Settings& s = powermode::settings();
    applyPolling();
    applyWifiSleep();
    APPLOG("power: %s, report every %lus, bms every %lus", powermode::modeName(),
                 static_cast<unsigned long>(s.publish_ms / 1000UL),
                 static_cast<unsigned long>(s.bms_poll_ms / 1000UL));
}

void startWifi() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.hostname(DEVICE_HOSTNAME);
    WiFi.setAutoReconnect(true);
    applyWifiSleep();
    beginProfile(0);
}

void startOta() {
    ArduinoOTA.setHostname(DEVICE_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        ota_in_progress = true;
        APPLOG("ota: start");
    });
    ArduinoOTA.onEnd([]() { APPLOG("ota: done, rebooting"); });
    ArduinoOTA.onError([](ota_error_t e) {
        ota_in_progress = false;
        APPLOG("ota: error %u", static_cast<unsigned>(e));
    });
    // No mDNS, here or anywhere: the responder parses every mDNS packet on the
    // LAN and allocates for each record, and with TLS up there are only a few kB
    // of heap. On the bench it ran out mid-parse (Unhandled C++ exception: OOM)
    // and the board hung in the crash handler. Use the IP, or the DHCP name
    // through the router (dalybms.lan), for the dashboard and OTA.
    ArduinoOTA.begin(false);
}

// Reboot if either link has been dead long enough that we are no longer doing
// our job. Better a 3-second outage than a bridge that silently serves stale
// numbers to a low-SOC alarm.
void checkWatchdogs() {
#if WIFI_WATCHDOG_MS > 0
    if (wifi_down_since != 0 && (millis() - wifi_down_since) > WIFI_WATCHDOG_MS) {
        APPLOG("watchdog: wifi down too long, restarting");
        delay(50);
        ESP.restart();
    }
#endif
#if BMS_WATCHDOG_MS > 0
    if (bms.secondsSinceGoodFrame() != 65535 &&
        static_cast<uint32_t>(bms.secondsSinceGoodFrame()) * 1000UL > BMS_WATCHDOG_MS) {
        APPLOG("watchdog: bms silent too long, restarting");
        delay(50);
        ESP.restart();
    }
#endif
}

} // namespace

void setup() {
    // First of all: in the middle of a storage sleep this wake is only a stop on
    // the way to the next one, and it should cost as little as possible.
    powermode::sleepIfMidChain();

    // 9600 8N1 to the BMS. Do not print to this port. TX only: replies arrive
    // on bms_rx, so GPIO3 is left alone.
    Serial.begin(9600, SERIAL_8N1, SERIAL_TX_ONLY);
    bms_rx.begin(9600, SWSERIAL_8N1, kBmsRxPin, -1, false, kBmsRxBuffer);

    // getResetInfo() adds the exception number and addresses after a crash, which
    // is what addr2line needs; for a normal reset it is the same as the reason.
    APPLOG("boot: daly-bms bridge, reset=%s", ESP.getResetInfo().c_str());
    // That line is cut at the log's line length, which loses the addresses. They
    // are what places a crash in firmware.elf, so give them a line of their own.
    const rst_info* ri = ESP.getResetInfoPtr();
    if (ri->reason == REASON_EXCEPTION_RST || ri->reason == REASON_SOFT_WDT_RST ||
        ri->reason == REASON_WDT_RST) {
        APPLOG("boot: exc %u epc %08lx %08lx %08lx addr %08lx",
               static_cast<unsigned>(ri->exccause), static_cast<unsigned long>(ri->epc1),
               static_cast<unsigned long>(ri->epc2), static_cast<unsigned long>(ri->epc3),
               static_cast<unsigned long>(ri->excvaddr));
    }

    // Before anything else touches the radio: if the guard put us to sleep, we
    // are up only to read the pack voltage and decide whether to go back down.
    const bool recheck = voltguard::begin();

    if (recheck) {
        // The recheck wants an answer inside GUARD_RECHECK_SECONDS whatever the
        // power mode says, so it polls at the configured fast rate.
        bms.begin(&bms_port, POLL_INTERVAL_MS, STALE_AFTER_MS);
        return;
    }

    powermode::begin();
    bms.begin(&bms_port, powermode::settings().bms_poll_ms, powermode::settings().bms_stale_ms);

    startWifi();
    startOta();

    ha_mqtt::begin(&bms);

    webserver::begin(&bms);

    APPLOG("boot: setup done, heap=%u block=%u", ESP.getFreeHeap(),
                 ESP.getMaxFreeBlockSize());
}

void loop() {
    // Recheck runs a stripped-down loop: BMS only, no radio, no watchdogs
    // (a silent BMS here means sleep again, not reboot). voltguard::loop() ends
    // this boot either by sleeping or by restarting into the normal path.
    if (voltguard::inRecheck()) {
        bms.loop();
        voltguard::loop(bms.data(), bms.valid());
        yield();
        return;
    }

    ArduinoOTA.handle();
    if (ota_in_progress) return; // do not touch the BMS mid-flash

    bms.loop();

    // Track WiFi state transitions so the watchdog has something to measure.
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifi_was_up) {
            APPLOG("wifi: up, ip=%s rssi=%d heap=%u block=%u",
                         WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap(),
                         ESP.getMaxFreeBlockSize());
            wifi_was_up = true;
            ha_mqtt::selectServer(kProfiles[profile_idx].external);
        }
        wifi_down_since = 0;
    } else {
        if (wifi_down_since == 0) {
            wifi_down_since = millis();
            // Only a real drop is worth logging; the initial connect is not.
            if (wifi_was_up) {
                APPLOG("wifi: lost");
                beginProfile(0); // start the search over, external first
            }
            wifi_was_up = false;
        } else if (millis() - last_wifi_attempt > kWifiRetryMs) {
            // This network did not come up in time; try the next one.
            beginProfile((profile_idx + 1) % kProfileCount);
        }
    }

    ha_mqtt::loop();
    webserver::loop();

    checkWatchdogs();

    // Also after the OTA early-return, for the same reason as the guard below.
    // May not come back: it restarts into a new mode, or ends a storage session
    // by deep-sleeping.
    if (powermode::service()) {
        applyMode();
        ha_mqtt::publishNow();
    }

    // Last, and after the OTA early-return above, so an update in flight is
    // never cut off mid-write by a sleep. This call may not come back.
    voltguard::loop(bms.data(), bms.valid());
    applyPolling();

    // Follow the MQTT link into and out of the slow radio (see applyWifiSleep).
    trackSettled();
    if (wantSlowRadio() != radio_slow) applyWifiSleep();

    // Light sleep only: park here so the SDK can put the CPU to sleep. Not while a
    // BMS request is out, because the reply arrives by interrupt on a core that
    // would be asleep. Network traffic ends the nap early. Otherwise just yield.
    if (radio_slow && powermode::settings().light_sleep && !bms.busy()) {
        const uint32_t nap = min(kIdleSliceMs, bms.msUntilSweep());
        if (nap > 0) {
            delay(nap);
            return;
        }
    }
    yield();
}
