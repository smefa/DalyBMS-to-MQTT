#include "webserver.h"

#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>

#if __has_include("config.h")
#include "config.h"
#endif

#include "applog.h"
#include "crashlog.h"
#include "dashboard.h"
#include "evlog.h"
#include "ha_mqtt.h"
#include "mdnslite.h"
#include "powermode.h"
#include "settings.h"
#include "voltguard.h"

namespace webserver {
namespace {

ESP8266WebServer server(80);
DalyBms* bms = nullptr;

const char* stateName(uint8_t s) {
    switch (s) {
    case daly::STATE_CHARGE: return "charging";
    case daly::STATE_DISCHARGE: return "discharging";
    default: return "idle";
    }
}

// Collects what ArduinoJson writes and hands it to the client in small pieces.
class ChunkPrint : public Print {
public:
    explicit ChunkPrint(ESP8266WebServer& srv) : srv_(srv) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* data, size_t len) override {
        for (size_t i = 0; i < len; i++) {
            buf_[n_++] = static_cast<char>(data[i]);
            if (n_ == sizeof buf_) flush();
        }
        return len;
    }
    void flush() override {
        if (n_ > 0) srv_.sendContent(buf_, n_);
        n_ = 0;
    }

private:
    ESP8266WebServer& srv_;
    char buf_[128];
    size_t n_ = 0;
};

uint32_t min_heap = UINT32_MAX;
uint32_t min_block = UINT32_MAX;

void handleStatus() {
    const daly::BmsData& d = bms->data();
    const bool valid = bms->valid();

    JsonDocument doc;
    doc["valid"] = valid;
    doc["stale_seconds"] = bms->secondsSincePack();
    doc["uptime_s"] = powermode::uptimeS();
    doc["mode"] = powermode::modeName();
    doc["rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = WiFi.SSID();
    wifi["rssi"] = WiFi.RSSI();

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["connected"] = ha_mqtt::connected();
    mqtt["server"] = ha_mqtt::serverName();
    mqtt["host"] = ha_mqtt::serverHost();

    const float volts = d.pack_voltage_dv / 10.0f;
    const float amps = d.pack_current_da / 10.0f;
    JsonObject pack = doc["pack"].to<JsonObject>();
    pack["voltage"] = volts;
    pack["current"] = amps;
    pack["power"] = volts * amps;
    pack["soc"] = d.soc_dpct / 10.0f;
    if (d.cells_reported > 0) {
        uint32_t sum_mv = 0;
        for (uint8_t i = 0; i < d.cells_reported; i++) sum_mv += d.cell_mv[i];
        pack["cells_v"] = sum_mv / 1000.0f; // the BMS's own pack voltage has 0.1 V steps
    }

    JsonObject cells = doc["cells"].to<JsonObject>();
    cells["count"] = d.cell_count;
    cells["reported"] = d.cells_reported;
    cells["max_mv"] = d.max_cell_mv;
    cells["max_idx"] = d.max_cell_idx;
    cells["min_mv"] = d.min_cell_mv;
    cells["min_idx"] = d.min_cell_idx;
    cells["delta_mv"] = (d.max_cell_mv > d.min_cell_mv) ? (d.max_cell_mv - d.min_cell_mv) : 0;
    JsonArray mv = cells["mv"].to<JsonArray>();
    for (uint8_t i = 0; i < d.cells_reported; i++) {
        mv.add(d.cell_mv[i]);
    }

    JsonObject temps = doc["temps"].to<JsonObject>();
    temps["count"] = d.temp_sensor_count;
    temps["max"] = d.max_temp_c;
    temps["max_idx"] = d.max_temp_idx;
    temps["min"] = d.min_temp_c;
    temps["min_idx"] = d.min_temp_idx;

    JsonObject st = doc["status"].to<JsonObject>();
    st["state"] = stateName(d.charge_state);
    st["charge_mos"] = d.charge_mos;
    st["discharge_mos"] = d.discharge_mos;
    st["remaining_mah"] = d.remaining_mah;
    st["cycles"] = d.cycles;
    st["charger_present"] = d.charger_present;
    st["load_present"] = d.load_present;
    st["bms_life"] = d.bms_life;

    const voltguard::Status& g = voltguard::status();
    JsonObject guard = doc["guard"].to<JsonObject>();
    guard["state"] = voltguard::stateName();
    guard["low_for_s"] = g.low_for_s;
    guard["sleeps"] = g.sleeps;
    guard["trip_mv"] = g.trip_mv;
    guard["limit_cell_mv"] = g.limit_cell_mv;
    guard["limit_from_bms"] = g.limit_from_bms;

    // The BMS's own protection settings, read-only. 0 means it has not answered.
    JsonObject lim = doc["limits"].to<JsonObject>();
    lim["cell_high_l1_mv"] = d.cell_high_l1_mv;
    lim["cell_high_l2_mv"] = d.cell_high_l2_mv;
    lim["cell_low_l1_mv"] = d.cell_low_l1_mv;
    lim["cell_low_l2_mv"] = d.cell_low_l2_mv;
    lim["pack_high_l1_v"] = d.pack_high_l1_dv / 10.0f;
    lim["pack_high_l2_v"] = d.pack_high_l2_dv / 10.0f;
    lim["pack_low_l1_v"] = d.pack_low_l1_dv / 10.0f;
    lim["pack_low_l2_v"] = d.pack_low_l2_dv / 10.0f;

    const char* ws = "none";
    switch (bms->writeState()) {
    case DalyBms::WriteState::Queued: ws = "queued"; break;
    case DalyBms::WriteState::Verifying: ws = "verifying"; break;
    case DalyBms::WriteState::Done: ws = "done"; break;
    case DalyBms::WriteState::Unchanged: ws = "unchanged"; break;
    case DalyBms::WriteState::Mismatch: ws = "mismatch"; break;
    default: break;
    }
    lim["profile"] = settings::currentProfile(d);
    for (settings::Profile p : {settings::Profile::Normal, settings::Profile::Storage}) {
        JsonObject o = lim["profiles"][settings::profileName(p)].to<JsonObject>();
        o["high_v"] = settings::profileHighDv(p) / 10.0f;
        o["low_v"] = settings::profileLowDv(p) / 10.0f;
    }
    lim["write"] = ws;
    lim["sleep_timeout_s"] = d.have_sleep ? static_cast<int>(d.sleep_timeout_s) : 0;

    JsonObject stats = doc["stats"].to<JsonObject>();
    stats["frames_ok"] = bms->framesOk();
    stats["timeouts"] = bms->timeouts();
    stats["checksum_errors"] = bms->checksumErrors();
    stats["sweeps"] = bms->sweeps();
    JsonObject by_cmd = stats["by_cmd"].to<JsonObject>();
    for (uint8_t i = 0; i < DalyBms::kCommandCount; i++) {
        char key[5];
        snprintf(key, sizeof key, "%02X", DalyBms::commandAt(i));
        JsonArray a = by_cmd[key].to<JsonArray>();
        a.add(bms->timeoutsFor(i));
        a.add(bms->checksumErrorsFor(i));
    }
    stats["free_heap"] = ESP.getFreeHeap();

    if (doc.overflowed()) {
        server.send(503, "text/plain", "out of memory\n");
        return;
    }
    // Streamed, not built into a String first: with TLS up there is only a few kB
    // of heap, and the String would be a second copy of the whole document.
    server.setContentLength(measureJson(doc));
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "");
    ChunkPrint chunks(server);
    serializeJson(doc, chunks);
    chunks.flush();
}

void handleLog() {
    String out;
    out.reserve(applog::LINES * applog::LINE_LEN);
    applog::dump(out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", out);
}

// Plain HTTP: the PIN crosses the network in the clear, so it only keeps out
// accidents and casual visitors on the LAN.
void handleLimits() {
    static const char* const kCell[4] = {"cell_high_l1_mv", "cell_high_l2_mv", "cell_low_l1_mv",
                                         "cell_low_l2_mv"};
    static const char* const kPack[4] = {"pack_high_l1_v", "pack_high_l2_v", "pack_low_l1_v",
                                         "pack_low_l2_v"};
    settings::Request req;
    bool bad = false;
    // An empty field means "leave as is"; anything else must be a whole number.
    auto field = [&](const char* name, double& v) {
        if (!server.hasArg(name) || server.arg(name).length() == 0) return false;
        if (!settings::parseNumber(server.arg(name).c_str(), v)) bad = true;
        return true;
    };
    double v = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if (field(kCell[i], v)) req.cell[i] = settings::clampRound(v);
        if (field(kPack[i], v)) req.pack[i] = settings::packFromVolts(v);
    }
    if (field("sleep_timeout_s", v)) req.sleep_s = settings::clampRound(v);
    if (bad) {
        server.send(400, "text/plain", "not a number (use a decimal point)\n");
        return;
    }
    settings::Result r;
    if (server.hasArg("profile")) {
        settings::Profile p;
        const String& name = server.arg("profile");
        bool other = req.sleep_s != settings::kUnset;
        for (uint8_t i = 0; i < 4; i++) {
            other = other || req.cell[i] != settings::kUnset || req.pack[i] != settings::kUnset;
        }
        if (other) {
            server.send(400, "text/plain", "send a profile on its own\n");
            return;
        }
        if (!settings::parseProfile(name.c_str(), name.length(), p)) {
            server.send(400, "text/plain", "profile must be normal or storage\n");
            return;
        }
        r = settings::applyProfile(*bms, server.arg("pin").c_str(), p);
    } else {
        r = settings::apply(*bms, server.arg("pin").c_str(), req);
    }
    int code = 200;
    switch (r) {
    case settings::Result::Queued: code = 202; break;
    case settings::Result::BadPin: code = 403; break;
    case settings::Result::Locked: code = 429; break;
    case settings::Result::Busy:
    case settings::Result::NoLimits: code = 409; break;
    default: code = 400; break;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "text/plain", String(settings::resultText(r)) + "\n");
}

void handleReboot() {
#ifdef WEB_ADMIN_PASSWORD
    // Plain HTTP, so the password is readable on the wire: it is deliberately not
    // the OTA password, which would allow a firmware upload.
    if (!server.authenticate("admin", WEB_ADMIN_PASSWORD)) {
        server.requestAuthentication();
        return;
    }
#else
    server.send(403, "text/plain", "reboot disabled: define WEB_ADMIN_PASSWORD\n");
    return;
#endif
    APPLOG("reboot requested over HTTP");
    server.send(200, "text/plain", "rebooting\n");
    delay(200);
    ESP.restart();
}

} // namespace

void begin(DalyBms* b) {
    bms = b;

    server.on("/", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "public, max-age=600");
        server.send_P(200, PSTR("text/html; charset=utf-8"), DASHBOARD_HTML);
    });
    server.on("/status.json", HTTP_GET, handleStatus);
    server.on("/log", HTTP_GET, handleLog);
    // Tiny fixed-size answer for when the heap is too low to build the others.
    server.on("/heap", HTTP_GET, []() {
        char b[208];
        // minheap/minblock are low-water marks since boot, sampled every pass of loop(),
        // so a dip between two polls still shows. radio is the WiFi sleep mode in force.
        // mdns is packets received / queries for our name / answers sent / answers dropped.
        const WiFiSleepType_t sleep = WiFi.getSleepMode();
        snprintf(b, sizeof b,
                 "heap=%u block=%u up=%lu mqtt=%d minheap=%u minblock=%u radio=%s rssi=%d silent=%lu "
                 "mdns=%lu/%lu/%lu/%lu\n",
                 ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), millis() / 1000UL,
                 ha_mqtt::connected() ? 1 : 0, min_heap, min_block,
                 sleep == WIFI_LIGHT_SLEEP ? "light" : sleep == WIFI_MODEM_SLEEP ? "modem" : "none",
                 WiFi.RSSI(), ha_mqtt::msSilent() / 1000UL,
                 static_cast<unsigned long>(mdnslite::received()),
                 static_cast<unsigned long>(mdnslite::queries()),
                 static_cast<unsigned long>(mdnslite::answers()),
                 static_cast<unsigned long>(mdnslite::skipped()));
        server.send(200, "text/plain", b);
    });
    // The RTC event ring (src/evlog.h) as hex, decoded by tools/collect.ps1.
    server.on("/events", HTTP_GET, []() {
        char b[320];
        evlog::dumpHex(b, sizeof b);
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/plain", b);
    });
    // Code addresses from the last crash dump (src/crashlog.h), for addr2line.
    server.on("/crash", HTTP_GET, []() {
        String out;
        crashlog::dump(out);
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/plain", out.length() ? out : String(F("none\n")));
    });
    server.on("/limits", HTTP_POST, handleLimits);
    server.on("/reboot", HTTP_POST, handleReboot);
    server.onNotFound([]() { server.send(404, "text/plain", "not found\n"); });

    server.begin();
    APPLOG("http server on :80");
}

void loop() {
    server.handleClient();
    // Max block walks the free list, so not on every pass.
    static uint32_t sampled_at = 0;
    min_heap = min<uint32_t>(min_heap, ESP.getFreeHeap());
    if (millis() - sampled_at >= 250) {
        sampled_at = millis();
        min_block = min<uint32_t>(min_block, ESP.getMaxFreeBlockSize());
    }
}

} // namespace webserver
