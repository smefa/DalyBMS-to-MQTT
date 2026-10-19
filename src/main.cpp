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
// BMS request line. Use APPLOG() and read http://dalybms.local/log.

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
extern "C" {
#include <user_interface.h>
}
#ifdef DEBUG_ESP_HWDT_NOEXTRA4K
#include <hwdt_app_entry.h>
#endif

#if __has_include("config.h")
#include "config.h"
#else
#error "src/config.h is missing. Copy src/config.h.example to src/config.h and fill it in."
#endif

#include "applog.h"
#include "bmswake.h"
#include "crashlog.h"
#include "daly_bms.h"
#include "evlog.h"
#include "ha_mqtt.h"
#include "mdnslite.h"
#include "powermode.h"
#include "voltguard.h"
#include "webserver.h"

namespace {

DalyBms bms;

constexpr uint8_t kBmsRxPin = 14; // D5 <- BMS TX (white)
// 0x95 is the longest reply: one 13-byte frame per three cells, so 11 frames
// (143 bytes) for a 32-cell pack. The buffer has to hold all of it, because a TLS
// handshake can keep loop() from draining it for a good part of a second.
constexpr int kBmsRxBuffer = 160;

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

// A mode switch can shorten the BMS silence limit below how long the BMS has already
// been quiet (normal polls rarely), so the bridge gets time to take a sweep first.
constexpr uint32_t kModeSwitchGraceMs = 15000;
uint32_t bms_watchdog_hold_until = 0; // millis() deadline; 0 = no hold

uint32_t wifi_down_since = 0;
uint32_t wifi_up_at = 0;

// WiFi associated but MQTT showing no life (no state published, no broker traffic) for
// this long, or four report intervals if longer, means the stack is wedged: the board
// once sat silent for 35 min and then died in the WiFi driver. Restart, which also
// returns the fragmented heap.
constexpr uint32_t kMqttSilentMinMs = 10UL * 60UL * 1000UL;
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
// High also light-sleeps (POWER_HIGH_LIGHT_SLEEP): the radio still parks between
// DTIM beacons, and the CPU sleeps too while loop() is parked in delay() -- see
// the end of loop(). The CPU is most of what an idle bridge draws. Normal and
// storage deep-sleep between reports and stay in the plain modem-sleep session.
//
// A listen interval would stretch the radio's wakeups past DTIM, but the core only
// passes it to the SDK on NONOS SDK 3.x and this build uses the default SDK 2.2.x,
// where it is ignored, so no mode sets one.
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
        evlog::add(evlog::Radio, slow);
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
    // Also while there is no valid reading: a BMS that was asleep at boot answers late,
    // and with a slow poll the first numbers would take a whole interval to arrive.
    const bool urgent = g.state == voltguard::State::Counting || g.near_limit || !bms.valid();
    const uint32_t fast = POLL_INTERVAL_MS;
    // The tight stale limit goes with the fast poll: a slow poll would make every
    // reading stale long before the next sweep.
    const bool fast_now = urgent && fast < s.bms_poll_ms;
    // But it is only taken on a reading that already fits it. A high-mode reading is up
    // to a whole slow poll old, and switching at once would turn it stale on the spot
    // and flash "unavailable" in Home Assistant until the next sweep. The wait is
    // bounded (kTightGraceMs): a BMS that stays silent must still go stale, or the
    // guard would count down on old readings. Once taken it is kept while polling fast.
    constexpr uint32_t kTightGraceMs = 15000; // a fast sweep takes a few seconds
    static bool tight = false;
    static uint32_t fast_since = 0;
    if (!fast_now) {
        tight = false;
        fast_since = 0;
    } else {
        if (fast_since == 0) fast_since = millis() | 1;
        if (!tight && (static_cast<uint32_t>(bms.secondsSincePack()) * 1000UL < STALE_AFTER_MS ||
                       millis() - fast_since >= kTightGraceMs)) {
            tight = true;
        }
    }
    bms.setPolling(fast_now ? fast : s.bms_poll_ms, tight ? STALE_AFTER_MS : s.bms_stale_ms);
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
    ArduinoOTA.onEnd([]() {
        APPLOG("ota: done, rebooting");
#ifdef DEBUG_ESP_HWDT_NOEXTRA4K
        // The HWDT stack dump runs before the SDK, on whatever DRAM the last firmware
        // left, and trusts it when g_pcont and g_rom_stack still hold their usual
        // values. They sit at the same addresses in every build, so after an OTA it
        // walked a stale BearSSL stack pointer, crashed (exception 28 in
        // skip_stackguard) and did the same on every reset, since the crash leaves
        // that RAM untouched: blue LED, reset button useless, only a power cut
        // helped. A wrong g_rom_stack makes the next boot skip the dump; the core
        // sets it again there. A serial flash is not covered.
        g_rom_stack = nullptr;
#endif
    });
    ArduinoOTA.onError([](ota_error_t e) {
        ota_in_progress = false;
        APPLOG("ota: error %u", static_cast<unsigned>(e));
    });
    // Not the core's mDNS responder: it parses every mDNS packet on the LAN and
    // allocates for each record, and with TLS up there are only a few kB of heap.
    // On the bench it ran out mid-parse (Unhandled C++ exception: OOM) and the
    // board hung in the crash handler. The name is answered by src/mdnslite instead.
    ArduinoOTA.begin(false);
}

// Reboot if either link has been dead long enough that we are no longer doing
// our job. Better a 3-second outage than a bridge that silently serves stale
// numbers to a low-SOC alarm.
void checkWatchdogs() {
#if WIFI_WATCHDOG_MS > 0
    if (wifi_down_since != 0 && (millis() - wifi_down_since) > WIFI_WATCHDOG_MS) {
        APPLOG("watchdog: wifi down too long, restarting");
        evlog::add(evlog::WdogWifi);
        powermode::saveOffline(); // the restart must not forget how long it has been
        delay(50);
        ESP.restart();
    }
#endif
    if (wifi_was_up) {
        const uint32_t silent = min<uint32_t>(ha_mqtt::msSilent(), millis() - wifi_up_at);
        // Sleeping modes use publish_ms = 0xFFFFFFFF ("once per session"): no interval to
        // scale by, the minimum stands. Otherwise follow the real interval (capped so
        // the product cannot overflow). The broker only answers pings, so allow for those.
        const powermode::Settings& s = powermode::settings();
        const uint32_t publish_ms = s.publish_ms == 0xFFFFFFFFUL ? 0 : min<uint32_t>(s.publish_ms, 6UL * 3600000UL);
        const uint32_t limit = max<uint32_t>(
            kMqttSilentMinMs, 4UL * publish_ms + 4UL * min<uint32_t>(s.ping_ms, 3600000UL));
        if (silent > limit) {
            APPLOG("watchdog: mqtt silent %lus, restarting (heap=%u block=%u radio=%s)",
                   static_cast<unsigned long>(silent / 1000UL), ESP.getFreeHeap(),
                   ESP.getMaxFreeBlockSize(), radio_slow ? "slow" : "normal");
            evlog::add(evlog::WdogMqtt, static_cast<uint8_t>(min<uint32_t>(255, silent / 60000UL)));
            powermode::saveOffline();
            delay(50);
            ESP.restart();
        }
    }
#if BMS_WATCHDOG_MS > 0
    // Never shorter than the mode's own "stale" limit (three missed sweeps): a slow
    // poll is silence by design, and a fixed limit would reboot the bridge between
    // every pair of sweeps.
    const uint32_t bms_limit_ms = max<uint32_t>(BMS_WATCHDOG_MS, powermode::settings().bms_stale_ms);
    const bool held = bms_watchdog_hold_until != 0 &&
                      static_cast<int32_t>(millis() - bms_watchdog_hold_until) < 0;
    if (!held && bms.secondsSinceGoodFrame() != 65535 &&
        static_cast<uint32_t>(bms.secondsSinceGoodFrame()) * 1000UL > bms_limit_ms) {
        APPLOG("watchdog: bms silent too long, restarting");
        evlog::add(evlog::WdogBms);
        powermode::saveOffline(); // a restart loop must not keep the no-WiFi count at zero
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
    // A deep-sleep wake is a stop on a chain, not a boot worth a slot.
    if (ri->reason != REASON_DEEP_SLEEP_AWAKE) {
        evlog::add(evlog::Boot, static_cast<uint8_t>(ri->reason));
    }
    if (ri->reason == REASON_EXCEPTION_RST || ri->reason == REASON_SOFT_WDT_RST ||
        ri->reason == REASON_WDT_RST) {
        APPLOG("boot: exc %u epc %08lx %08lx %08lx addr %08lx",
               static_cast<unsigned>(ri->exccause), static_cast<unsigned long>(ri->epc1),
               static_cast<unsigned long>(ri->epc2), static_cast<unsigned long>(ri->epc3),
               static_cast<unsigned long>(ri->excvaddr));
    }
    if (const uint16_t kept = crashlog::begin()) {
        APPLOG("boot: crash dump kept (%u entries), see /crash", static_cast<unsigned>(kept));
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
    bmswake::begin();
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
        // Wait for a whole first sweep: deciding on 0x90 alone means no cell
        // voltages, cell count or BMS limits yet. A BMS that never completes one
        // runs into the recheck deadline and the guard sleeps again.
        voltguard::loop(bms.data(), bms.valid() && bms.complete());
        yield();
        return;
    }

    ArduinoOTA.handle();
    if (ota_in_progress) return; // do not touch the BMS mid-flash

    bms.loop();

    powermode::noteWifi(WiFi.status() == WL_CONNECTED);

    // Track WiFi state transitions so the watchdog has something to measure.
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifi_was_up) {
            APPLOG("wifi: up, ip=%s rssi=%d heap=%u block=%u",
                         WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap(),
                         ESP.getMaxFreeBlockSize());
            wifi_was_up = true;
            wifi_up_at = millis();
            evlog::add(evlog::WifiUp, static_cast<uint8_t>(-WiFi.RSSI()));
            ha_mqtt::selectServer(kProfiles[profile_idx].external);
        }
        wifi_down_since = 0;
    } else {
        if (wifi_down_since == 0) {
            wifi_down_since = millis();
            // Only a real drop is worth logging; the initial connect is not.
            if (wifi_was_up) {
                APPLOG("wifi: lost");
                evlog::add(evlog::WifiLost);
                beginProfile(0); // start the search over, external first
            }
            wifi_was_up = false;
        } else if (millis() - last_wifi_attempt > kWifiRetryMs) {
            // This network did not come up in time; try the next one.
            beginProfile((profile_idx + 1) % kProfileCount);
        }
    }

#if !defined(MDNS_RESPONDER) || MDNS_RESPONDER
    mdnslite::loop(WiFi.status() == WL_CONNECTED);
#endif

    // The BMS goes to sleep on its own, in any power mode (a storage bridge has also
    // been asleep for hours). Pulses at boot, then retries while the BMS stays silent.
    // A pulse is only worth something if a request follows before the BMS dozes off
    // again, and the next regular sweep may be a minute away.
    // "Answered" means a usable pack frame: a half-awake BMS still sends frames (the
    // pack one all zero), and counting those kept the wake pulse from ever firing.
    // Woken at the first unanswered 0x90, not when valid() runs out: a slow poll keeps
    // the data valid for three sweeps, and waiting that long meant the BMS was only
    // woken after Home Assistant had already been told "unavailable". Same in every
    // mode: a normal or storage session polls fast, so this fires within one sweep.
    // Until the first sweep has finished nothing has had a chance to answer yet, so
    // an awake BMS looked silent here and got a wake pulse at every boot. A silent one
    // finishes that sweep in about 1.5 s.
    const bool bms_answering = (bms.valid() && !bms.packMissed()) || bms.sweeps() == 0;
    if (bmswake::serviceStorage(bms_answering)) bms.requestSweep();

    ha_mqtt::loop();
    webserver::loop();

    checkWatchdogs();

    // After the OTA early-return above, so an update in flight is never cut off
    // mid-write by a sleep. This call may not come back.
    voltguard::loop(bms.data(), bms.valid());
    applyPolling();

    // After the guard, so a session about to end sees this pass's reading and does
    // not sleep through the start of a countdown. May not come back either: it
    // restarts into a new mode, or ends a normal/storage session by deep-sleeping.
    if (powermode::service()) {
        applyMode();
        bms.requestSweep();
        bms_watchdog_hold_until = (millis() + kModeSwitchGraceMs) | 1;
        ha_mqtt::publishNow();
    }

    // Follow the MQTT link into and out of the slow radio (see applyWifiSleep).
    trackSettled();
    if (wantSlowRadio() != radio_slow) applyWifiSleep();

    // A slow leak or fragmentation shows as a trend in these, kept across a reset.
    static uint32_t heap_logged_at = 0;
    if (millis() - heap_logged_at >= 600000UL) {
        heap_logged_at = millis();
        evlog::add(evlog::Heap, static_cast<uint8_t>(min<uint32_t>(255, ESP.getFreeHeap() >> 7)));
        evlog::add(evlog::Block, static_cast<uint8_t>(min<uint32_t>(255, ESP.getMaxFreeBlockSize() >> 7)));
    }

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
