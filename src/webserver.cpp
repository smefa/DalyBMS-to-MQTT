#include "webserver.h"

#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>

#include "applog.h"
#include "dashboard.h"
#include "ha_mqtt.h"
#include "powermode.h"
#include "voltguard.h"

namespace webserver {
namespace {

ESP8266WebServer server(80);
const DalyBms* bms = nullptr;

const char* stateName(uint8_t s) {
    switch (s) {
    case daly::STATE_CHARGE: return "charging";
    case daly::STATE_DISCHARGE: return "discharging";
    default: return "idle";
    }
}

void handleStatus() {
    const daly::BmsData& d = bms->data();
    const bool valid = bms->valid();

    JsonDocument doc;
    doc["valid"] = valid;
    doc["stale_seconds"] = bms->secondsSinceGoodFrame();
    doc["uptime_s"] = millis() / 1000;
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

    JsonObject stats = doc["stats"].to<JsonObject>();
    stats["frames_ok"] = bms->framesOk();
    stats["timeouts"] = bms->timeouts();
    stats["checksum_errors"] = bms->checksumErrors();
    stats["sweeps"] = bms->sweeps();
    stats["free_heap"] = ESP.getFreeHeap();

    String out;
    serializeJson(doc, out);
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", out);
}

void handleLog() {
    String out;
    out.reserve(applog::LINES * applog::LINE_LEN);
    applog::dump(out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", out);
}

void handleReboot() {
    APPLOG("reboot requested over HTTP");
    server.send(200, "text/plain", "rebooting\n");
    delay(200);
    ESP.restart();
}

} // namespace

void begin(const DalyBms* b) {
    bms = b;

    server.on("/", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "public, max-age=600");
        server.send_P(200, PSTR("text/html; charset=utf-8"), DASHBOARD_HTML);
    });
    server.on("/status.json", HTTP_GET, handleStatus);
    server.on("/log", HTTP_GET, handleLog);
    // Tiny fixed-size answer for when the heap is too low to build the others.
    server.on("/heap", HTTP_GET, []() {
        char b[96];
        snprintf(b, sizeof b, "heap=%u block=%u up=%lu mqtt=%d\n", ESP.getFreeHeap(),
                 ESP.getMaxFreeBlockSize(), millis() / 1000UL, ha_mqtt::connected() ? 1 : 0);
        server.send(200, "text/plain", b);
    });
    server.on("/reboot", HTTP_POST, handleReboot);
    server.onNotFound([]() { server.send(404, "text/plain", "not found\n"); });

    server.begin();
    APPLOG("http server on :80");
}

void loop() {
    server.handleClient();
}

} // namespace webserver
