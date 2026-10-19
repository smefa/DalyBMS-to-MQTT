#include "ha_mqtt.h"

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WebSocketsClient.h>

#include "applog.h"
#include "config.h"
#include "evlog.h"
#include "mqttlite.h"
#include "powermode.h"
#include "settings.h"
#include "voltguard.h"

namespace ha_mqtt {
namespace {

// Pings come from powermode::settings().ping_ms, on their own timer: a QoS 0
// publish gets no answer, so only a PINGREQ makes the broker send something back
// for a proxy's read timeout to see. The keepalive sent in CONNECT is twice the
// ping interval (the broker's window is 1.5x that), never below kMinKeepaliveS, so
// a mode with fast pings keeps fast dead-board detection.
constexpr uint32_t kMinKeepaliveS = 60;
constexpr uint32_t kReconnectMs = 5000;
constexpr uint32_t kFreshForReportS = 30; // pack data older than this is not reported as current
constexpr size_t kBufSize = 1200;        // largest packet: 32-cell state or a discovery config

// Do not publish the very first state until the BMS has answered, or give up
// waiting after this long since boot: a BMS that never answers is itself news.
constexpr uint32_t kFirstDataWaitMs = 15000;
// A BMS that answers 0x90 but not everything else must not hold the first report
// back for ever; after this the missing readings go out as the zeros they are.
constexpr uint32_t kLiveDataGiveUpMs = 60000;
// Gap between sweeps asked for by an overdue report. A sweep that times out ends at
// once, so without it a silent BMS is hit back to back (237 timeouts a minute seen).
constexpr uint32_t kReportSweepGapMs = 10000;
// A retained command arrives right behind the SUBACK. A storage session stays up
// this long after it to be sure it has seen it.
constexpr uint32_t kRetainedGraceMs = 1500;
constexpr uint16_t kSubscribePacketId = 1;
constexpr uint16_t kSubscribeLimitsPacketId = 2;
constexpr uint16_t kSubscribeProfilePacketId = 3;
constexpr uint8_t kSubscriptions = 3;

DalyBms* bms = nullptr;
WebSocketsClient ws;

uint8_t buf[kBufSize];
char status_topic[64];
char state_topic[64];
char mode_set_topic[64];
char limits_topic[64];
char limits_set_topic[64];
char limits_result_topic[64];
char profile_set_topic[64];

bool connected_ = false;
bool connect_sent_ = false;
bool connack_seen_ = false;
uint8_t subacks_ = 0;
bool suback_seen_ = false;     // the broker answered both our SUBSCRIBEs, either way
bool published_ = false;       // a state document has gone out on this connection
uint32_t suback_at_ = 0;
bool server_chosen_ = false;
bool server_external_ = false;
const char* server_host_ = "";
uint32_t last_ping_ = 0;
uint32_t keepalive_s_ = kMinKeepaliveS; // what the current connection announced
uint32_t last_state_ = 0;
uint32_t last_state_ok_ = 0; // when a state document last went out, or the CONNACK
uint32_t last_rx_ = 0;       // when the broker last sent anything, or the CONNACK
bool retry_pending_ = false;  // the last report did not go out; try again at retry_at_
uint32_t retry_at_ = 0;

// High mode only: the pack voltage is the sum of the cells (mV), averaged over the
// sweeps since the last report. The BMS's own pack voltage has only 0.1 V steps.
uint32_t volt_sum_mv_ = 0;
uint16_t volt_samples_ = 0;
uint32_t volt_sweep_ = 0; // bms->sweeps() when the last sample was taken

void sampleVoltage() {
    const uint32_t sweeps = bms->sweeps();
    if (sweeps == volt_sweep_) return;
    volt_sweep_ = sweeps;
    const daly::BmsData& d = bms->data();
    if (powermode::mode() != powercore::Mode::High || !bms->valid() || d.cells_reported == 0 ||
        (d.cell_count != 0 && d.cells_reported != d.cell_count)) {
        return;
    }
    uint32_t sum = 0;
    for (uint8_t i = 0; i < d.cells_reported; i++) sum += d.cell_mv[i];
    volt_sum_mv_ += sum;
    volt_samples_++;
}

// Pack voltage for a report: the average of the samples when there are any; otherwise
// (the first report after boot, or a sleeping mode) the sum of the cells from the latest
// sweep. Only without a complete set of cells is it the BMS's own 0.1 V value.
float reportVoltage(const daly::BmsData& d) {
    if (volt_samples_ > 0) {
        return roundf(volt_sum_mv_ / static_cast<float>(volt_samples_) / 10.0f) / 100.0f;
    }
    if (d.cells_reported > 0 && (d.cell_count == 0 || d.cells_reported == d.cell_count)) {
        uint32_t sum = 0;
        for (uint8_t i = 0; i < d.cells_reported; i++) sum += d.cell_mv[i];
        return roundf(sum / 10.0f) / 100.0f;
    }
    return d.pack_voltage_dv / 10.0f;
}

// Discovery is sent one config per loop() pass so the radio and the BMS driver
// are never starved by a burst of 40 publishes.
uint8_t discovery_next_ = 0;
bool discovery_pending_ = false;

uint32_t limits_sig_ = 0;       // what the last retained limits document said
uint16_t result_serial_ = 0;    // the write whose outcome is still owed to limits/result
bool result_owed_ = false;

voltguard::State last_guard_ = voltguard::State::Normal;
const char* last_profile_ = "";
bool last_bms_ok_ = false;
// A change seen while the first report is held back, or whose report failed, stays
// owed until a report has actually gone out.
bool urgent_owed_ = false;  // guard state changed: goes out as soon as first_ready
bool change_owed_ = false;  // profile or "BMS fresh" changed: goes out with data_ready
uint32_t report_sweep_at_ = 0; // when an overdue report last asked for a sweep
bool config_blocked_ = false; // bad TLS pin: never started, so never loop() the socket

#if MQTT_TLS
// "AB:CD:..." / "abcd..." -> 20 bytes. Separators are skipped. False unless
// exactly 40 hex digits were found.
bool parseFingerprint(const char* s, uint8_t out[20]) {
    uint8_t digits = 0;
    for (; *s; s++) {
        int v;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else if (*s == ':' || *s == ' ') continue;
        else return false;
        if (digits >= 40) return false;
        if (digits % 2 == 0) out[digits / 2] = static_cast<uint8_t>(v << 4);
        else out[digits / 2] |= static_cast<uint8_t>(v);
        digits++;
    }
    return digits == 40;
}
#endif

// ---- Entities --------------------------------------------------------------

struct Entity {
    const char* component; // "sensor" or "binary_sensor"
    const char* id;        // object id, also the JSON key in the state document
    const char* name;
    const char* unit;      // nullptr for none
    const char* dev_class; // nullptr for none
    const char* state_class;
    bool diagnostic;
};

constexpr Entity kEntities[] = {
    {"sensor", "voltage", "Pack voltage", "V", "voltage", "measurement", false},
    {"sensor", "current", "Pack current", "A", "current", "measurement", false},
    {"sensor", "power", "Pack power", "W", "power", "measurement", false},
    {"sensor", "soc", "State of charge", "%", "battery", "measurement", false},
    {"sensor", "remaining_ah", "Remaining capacity", "Ah", nullptr, "measurement", false},
    {"sensor", "cell_max_mv", "Highest cell", "mV", "voltage", "measurement", false},
    {"sensor", "cell_min_mv", "Lowest cell", "mV", "voltage", "measurement", false},
    {"sensor", "cell_delta_mv", "Cell imbalance", "mV", "voltage", "measurement", false},
    {"sensor", "temp_max", "Highest temperature", "\xC2\xB0""C", "temperature", "measurement", false},
    {"sensor", "temp_min", "Lowest temperature", "\xC2\xB0""C", "temperature", "measurement", false},
    {"sensor", "state", "Battery state", nullptr, nullptr, nullptr, false},
    {"sensor", "cycles", "Cycles", nullptr, nullptr, "total_increasing", false},
    {"binary_sensor", "charge_mos", "Charge MOSFET", nullptr, nullptr, nullptr, false},
    {"binary_sensor", "discharge_mos", "Discharge MOSFET", nullptr, nullptr, nullptr, false},
    // Read "bms_ok" before trusting the rest: a silent BMS leaves the last
    // values in place, and only this and stale_s say so.
    {"binary_sensor", "bms_ok", "BMS data fresh", nullptr, "connectivity", nullptr, true},
    {"sensor", "stale_s", "Seconds since BMS frame", "s", nullptr, "measurement", true},
    // "counting" means the low-voltage guard is about to deep-sleep the bridge.
    // Alarm on that: the silence that follows is indistinguishable from a crash.
    {"sensor", "guard", "Low-voltage guard", nullptr, nullptr, nullptr, true},
    {"sensor", "guard_low_s", "Guard seconds spent low", "s", nullptr, "measurement", true},
    {"sensor", "rssi", "WiFi signal", "dBm", "signal_strength", "measurement", true},
    // Text, not a number: a duration sensor has one fixed unit, and this reads as
    // minutes first, then hours, then days.
    {"sensor", "uptime", "Uptime", nullptr, nullptr, nullptr, true},
    // Where the dashboard and OTA are when <hostname>.local does not resolve.
    {"sensor", "ip", "IP address", nullptr, nullptr, nullptr, true},
};
constexpr uint8_t kEntityCount = sizeof(kEntities) / sizeof(kEntities[0]);

// ---- Sending ---------------------------------------------------------------

bool sendRaw(size_t n) {
    if (n == 0) return false;
    return ws.sendBIN(buf, n);
}

bool publish(const char* topic, const char* payload, size_t len, bool retain) {
    const size_t n = mqttlite::encodePublish(buf, sizeof buf, topic,
                                             reinterpret_cast<const uint8_t*>(payload), len, retain);
    if (n == 0) {
        APPLOG("mqtt: %s does not fit in %u bytes", topic, static_cast<unsigned>(kBufSize));
        return false;
    }
    return sendRaw(n);
}

bool publishStr(const char* topic, const char* payload, bool retain) {
    return publish(topic, payload, strlen(payload), retain);
}

// ---- Discovery -------------------------------------------------------------

// In the sleeping modes the bridge is silent between reports, and a clean
// disconnect leaves "online" retained, so a bridge that died asleep would look
// healthy forever. Expiring the values would catch that, but it also blanks them
// to "unavailable" after one late report, so the sleeping modes keep the last
// values instead (0 = no expiry); the stale_s sensor shows how old they are.
uint32_t expireAfterS() { return powermode::sleeps() ? 0 : powermode::sleepIntervalS() * 2; }

void discoveryTopic(char* out, size_t cap, const char* component, const char* id) {
    snprintf(out, cap, "%s/%s/%s/%s/config", HA_DISCOVERY_PREFIX, component, DEVICE_HOSTNAME, id);
}

void fillDevice(JsonDocument& doc) {
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"][0] = DEVICE_HOSTNAME;
    dev["name"] = "DALY BMS";
    dev["mf"] = "DALY";
    dev["mdl"] = "BMS via ESP8266 bridge";
}

void baseConfig(JsonDocument& doc, const char* id, const char* name) {
    char uniq[64];
    snprintf(uniq, sizeof uniq, "%s_%s", DEVICE_HOSTNAME, id);
    doc["name"] = name;
    doc["uniq_id"] = uniq;
    doc["stat_t"] = state_topic;
    doc["avty_t"] = status_topic;
    fillDevice(doc);
}

// Available only while the bridge is online AND the state says the BMS data is fresh.
// Without it a silent BMS leaves zeros or the last values in the charts; with it the
// entity goes "unavailable", which Home Assistant leaves out of history graphs.
void requireFreshBms(JsonDocument& doc) {
    doc.remove("avty_t");
    JsonArray a = doc["avty"].to<JsonArray>();
    a.add<JsonObject>()["t"] = status_topic;
    JsonObject b = a.add<JsonObject>();
    b["t"] = state_topic;
    b["val_tpl"] = "{{ value_json.bms_ok }}";
    b["pl_avail"] = "ON";
    b["pl_not_avail"] = "OFF";
    doc["avty_mode"] = "all";
}

bool publishEntityDiscovery(const Entity& e) {
    JsonDocument doc;
    baseConfig(doc, e.id, e.name);
    if (!e.diagnostic) requireFreshBms(doc);
    char tpl[48];
    snprintf(tpl, sizeof tpl, "{{ value_json.%s }}", e.id);
    doc["val_tpl"] = tpl;
    if (e.unit) doc["unit_of_meas"] = e.unit;
    if (e.dev_class) doc["dev_cla"] = e.dev_class;
    if (e.state_class) doc["stat_cla"] = e.state_class;
    if (e.diagnostic) doc["ent_cat"] = "diagnostic";
    if (expireAfterS() > 0) doc["exp_aft"] = expireAfterS();
    // The report carries two decimals (cell-sum average); without this HA may round.
    if (strcmp(e.id, "voltage") == 0) doc["sug_dsp_prc"] = 2;

    char topic[128];
    discoveryTopic(topic, sizeof topic, e.component, e.id);
    char out[768];
    if (doc.overflowed()) return false;
    const size_t n = serializeJson(doc, out, sizeof out);
    if (n >= sizeof out) return false;
    return publish(topic, out, n, true);
}

// The "select" that switches power mode. Home Assistant retains what it sends, so
// a bridge that is asleep in storage mode finds the command waiting when it wakes.
bool publishModeDiscovery() {
    JsonDocument doc;
    baseConfig(doc, "power_mode", "Power mode");
    doc["val_tpl"] = "{{ value_json.mode }}";
    doc["cmd_t"] = mode_set_topic;
    doc["retain"] = true;
    doc["ent_cat"] = "config";
    JsonArray options = doc["options"].to<JsonArray>();
    options.add("storage");
    options.add("normal");
    options.add("high");

    char topic[128];
    discoveryTopic(topic, sizeof topic, "select", "power_mode");
    char out[640];
    if (doc.overflowed()) return false;
    const size_t n = serializeJson(doc, out, sizeof out);
    if (n >= sizeof out) return false;
    return publish(topic, out, n, true);
}

// The "select" that picks the pack voltage profile. Not retained: a retained command
// would be replayed on every reconnect and rewrite the BMS each time.
bool publishProfileDiscovery() {
    JsonDocument doc;
    baseConfig(doc, "pack_profile", "Pack voltage profile");
    // "custom" and "unknown" are not options; "None" makes the select show unknown.
    doc["val_tpl"] =
        "{{ value_json.pack_profile if value_json.pack_profile in ['normal','storage'] else none }}";
    doc["cmd_t"] = profile_set_topic;
    doc["ent_cat"] = "config";
    JsonArray options = doc["options"].to<JsonArray>();
    options.add("normal");
    options.add("storage");

    char topic[128];
    discoveryTopic(topic, sizeof topic, "select", "pack_profile");
    char out[640];
    if (doc.overflowed()) return false;
    const size_t n = serializeJson(doc, out, sizeof out);
    if (n >= sizeof out) return false;
    return publish(topic, out, n, true);
}

bool publishCellDiscovery(uint8_t cell) {
    char id[12], name[16];
    snprintf(id, sizeof id, "cell_%u", cell);
    snprintf(name, sizeof name, "Cell %u", cell);

    JsonDocument doc;
    baseConfig(doc, id, name);
    requireFreshBms(doc);
    char tpl[48];
    snprintf(tpl, sizeof tpl, "{{ value_json.%s }}", id);
    doc["val_tpl"] = tpl;
    doc["unit_of_meas"] = "mV";
    doc["dev_cla"] = "voltage";
    doc["stat_cla"] = "measurement";
    doc["ent_cat"] = "diagnostic";
    if (expireAfterS() > 0) doc["exp_aft"] = expireAfterS();

    char topic[128];
    discoveryTopic(topic, sizeof topic, "sensor", id);
    char out[768];
    if (doc.overflowed()) return false;
    const size_t n = serializeJson(doc, out, sizeof out);
    if (n >= sizeof out) return false;
    return publish(topic, out, n, true);
}

// Sends the next config, if any. Order: the fixed entities, the power-mode select,
// then one per cell. Cells exist only for the cells the BMS reported, so they
// wait for the first good frame.
void stepDiscovery() {
    if (!discovery_pending_) return;

    if (discovery_next_ < kEntityCount) {
        if (publishEntityDiscovery(kEntities[discovery_next_])) discovery_next_++;
        return;
    }
    if (discovery_next_ == kEntityCount) {
        if (publishModeDiscovery()) discovery_next_++;
        return;
    }

    if (discovery_next_ == kEntityCount + 1) {
        if (publishProfileDiscovery()) discovery_next_++;
        return;
    }

    const uint8_t cells = bms->data().cell_count;
    if (cells == 0) return; // keep waiting for the BMS to tell us
    const uint8_t cell = discovery_next_ - kEntityCount - 1; // 1-based: the two selects come first
    if (cell <= cells) {
        if (publishCellDiscovery(cell)) discovery_next_++;
        return;
    }
    discovery_pending_ = false;
    APPLOG("mqtt: discovery sent, %u entities + mode select + %u cells", kEntityCount, cells);
}

// ---- State -----------------------------------------------------------------

const char* stateName(uint8_t s) {
    switch (s) {
    case daly::STATE_CHARGE: return "charging";
    case daly::STATE_DISCHARGE: return "discharging";
    default: return "idle";
    }
}

bool publishState() {
    const daly::BmsData& d = bms->data();
    const voltguard::Status& g = voltguard::status();

    const float volts = reportVoltage(d);
    const float amps = d.pack_current_da / 10.0f;

    JsonDocument doc;
    doc["voltage"] = volts;
    doc["current"] = amps;
    doc["power"] = volts * amps;
    doc["soc"] = d.soc_dpct / 10.0f;
    doc["remaining_ah"] = d.remaining_mah / 1000.0f;
    doc["cell_max_mv"] = d.max_cell_mv;
    doc["cell_min_mv"] = d.min_cell_mv;
    doc["cell_delta_mv"] = d.max_cell_mv > d.min_cell_mv ? d.max_cell_mv - d.min_cell_mv : 0;
    doc["temp_max"] = d.max_temp_c;
    doc["temp_min"] = d.min_temp_c;
    doc["state"] = stateName(d.charge_state);
    doc["cycles"] = d.cycles;
    doc["charge_mos"] = d.charge_mos ? "ON" : "OFF";
    doc["discharge_mos"] = d.discharge_mos ? "ON" : "OFF";
    doc["bms_ok"] = bms->valid() ? "ON" : "OFF";
    doc["stale_s"] = bms->secondsSincePack();
    doc["guard"] = voltguard::stateName();
    doc["guard_low_s"] = g.low_for_s;
    doc["rssi"] = WiFi.RSSI();
    const IPAddress ip = WiFi.localIP();
    char ip_str[16];
    snprintf(ip_str, sizeof ip_str, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    doc["ip"] = ip_str;
    char up[24];
    powercore::formatUptime(powermode::uptimeS(), up, sizeof up);
    doc["uptime"] = up;
    doc["mode"] = powermode::modeName();
    doc["pack_profile"] = settings::currentProfile(d);
    for (uint8_t i = 0; i < d.cells_reported; i++) {
        char key[12];
        snprintf(key, sizeof key, "cell_%u", i + 1);
        doc[key] = d.cell_mv[i];
    }

    // Serialised straight into the send buffer, behind room for the PUBLISH header,
    // rather than into a buffer of its own: with TLS up the heap is a few kB and a
    // second 1 kB buffer is a lot. (The ESP8266 stack is only 4 kB, so not on that.)
    constexpr size_t kHeaderRoom = 128;
    if (doc.overflowed()) return false;
    char* out = reinterpret_cast<char*>(buf) + kHeaderRoom;
    const size_t n = serializeJson(doc, out, kBufSize - kHeaderRoom);
    if (n >= kBufSize - kHeaderRoom) return false; // truncated, so not valid JSON
    // Retained in the sleeping modes: the next report is minutes to a day away, and
    // without it a restarted Home Assistant shows every entity as unknown until then.
    if (publish(state_topic, out, n, powermode::sleeps())) {
        published_ = true;
        last_state_ok_ = millis();
        volt_sum_mv_ = 0;
        volt_samples_ = 0;
        return true;
    }
    return false;
}

// ---- Incoming --------------------------------------------------------------

// ---- Limits ----------------------------------------------------------------

bool publishResult(const char* text) {
    return publishStr(limits_result_topic, text, false);
}

uint32_t limitsSignature(const daly::BmsData& d) {
    uint32_t h = 2166136261u;
    const uint16_t v[9] = {d.cell_high_l1_mv, d.cell_high_l2_mv, d.cell_low_l1_mv, d.cell_low_l2_mv,
                           d.pack_high_l1_dv, d.pack_high_l2_dv, d.pack_low_l1_dv, d.pack_low_l2_dv,
                           d.sleep_timeout_s};
    for (uint16_t x : v) h = (h ^ x) * 16777619u;
    return h | 1; // never 0, which means "not published yet"
}

// Same key names as the command and status.json, so what is read can be written back.
void publishLimits() {
    const daly::BmsData& d = bms->data();
    if (d.cell_low_l1_mv == 0 && d.pack_low_l1_dv == 0) return; // BMS has not answered
    const uint32_t sig = limitsSignature(d);
    if (sig == limits_sig_) return;

    JsonDocument doc;
    doc["cell_high_l1_mv"] = d.cell_high_l1_mv;
    doc["cell_high_l2_mv"] = d.cell_high_l2_mv;
    doc["cell_low_l1_mv"] = d.cell_low_l1_mv;
    doc["cell_low_l2_mv"] = d.cell_low_l2_mv;
    doc["pack_high_l1_v"] = d.pack_high_l1_dv / 10.0f;
    doc["pack_high_l2_v"] = d.pack_high_l2_dv / 10.0f;
    doc["pack_low_l1_v"] = d.pack_low_l1_dv / 10.0f;
    doc["pack_low_l2_v"] = d.pack_low_l2_dv / 10.0f;
    if (d.have_sleep) doc["sleep_timeout_s"] = d.sleep_timeout_s;
    char out[300];
    if (doc.overflowed()) return;
    const size_t n = serializeJson(doc, out, sizeof out);
    if (n >= sizeof out) return;
    if (publish(limits_topic, out, n, true)) limits_sig_ = sig;
}

void handleLimitsCommand(const mqttlite::Publish& p) {
    const settings::Result r =
        settings::applyJson(*bms, reinterpret_cast<const char*>(p.payload), p.payload_len);
    publishResult(settings::resultText(r));
    if (r == settings::Result::Queued) {
        result_serial_ = bms->writeSerial();
        result_owed_ = true;
    }
}

// Tells the sender how its accepted command ended.
void reportWriteOutcome() {
    if (!result_owed_ || bms->writeSerial() != result_serial_) return;
    const char* text = nullptr;
    switch (bms->writeState()) {
    case DalyBms::WriteState::Done: text = "done"; break;
    case DalyBms::WriteState::Unchanged: text = "unchanged"; break;
    case DalyBms::WriteState::Mismatch: text = "mismatch"; break;
    default: return;
    }
    if (publishResult(text)) result_owed_ = false;
}

void handlePublish(const mqttlite::Publish& p) {
    if (p.topic_len == strlen(limits_set_topic) &&
        memcmp(p.topic, limits_set_topic, p.topic_len) == 0) {
        handleLimitsCommand(p);
        return;
    }
    if (p.topic_len == strlen(profile_set_topic) &&
        memcmp(p.topic, profile_set_topic, p.topic_len) == 0) {
        settings::Profile prof;
        if (!settings::parseProfile(reinterpret_cast<const char*>(p.payload), p.payload_len, prof)) {
            APPLOG("mqtt: ignoring profile command");
            return;
        }
        const settings::Result r = settings::applyProfileTrusted(*bms, prof);
        publishResult(settings::resultText(r));
        if (r == settings::Result::Queued) {
            result_serial_ = bms->writeSerial();
            result_owed_ = true;
        }
        return;
    }
    if (p.topic_len != strlen(mode_set_topic) ||
        memcmp(p.topic, mode_set_topic, p.topic_len) != 0) {
        return;
    }
    powermode::Mode m;
    if (!powercore::parseMode(reinterpret_cast<const char*>(p.payload), p.payload_len, m)) {
        APPLOG("mqtt: ignoring mode command \"%.*s\"",
                     static_cast<int>(p.payload_len < 16 ? p.payload_len : 16),
                     reinterpret_cast<const char*>(p.payload));
        return;
    }
    powermode::request(m);
}

void onSuback(int rc) {
    if (++subacks_ >= kSubscriptions) suback_seen_ = true;
    suback_at_ = millis();
    if (rc < 0 || rc >= 0x80) {
        APPLOG("mqtt: broker refused a subscribe (%s or %s); that command cannot be sent "
                     "over MQTT (does the broker user have read access?)",
                     mode_set_topic, limits_set_topic);
        // (profile_set_topic is the third)
    }
}

// ---- Connection ------------------------------------------------------------

void sendConnect() {
    mqttlite::ConnectOptions o;
    o.client_id = DEVICE_HOSTNAME;
    // 0 (no pings configured) tells the broker never to time this client out.
    const uint32_t ping_ms = powermode::settings().ping_ms;
    keepalive_s_ = ping_ms == 0 ? 0
                                : min<uint32_t>(65535, max<uint32_t>(kMinKeepaliveS, ping_ms / 500UL));
    o.keepalive_s = static_cast<uint16_t>(keepalive_s_);
    last_ping_ = millis();
    o.will_topic = status_topic;
    o.will_payload = "offline";
    o.will_retain = true;
#ifdef MQTT_USER
    o.username = MQTT_USER;
    o.password = MQTT_PASSWORD;
#endif
    connack_seen_ = false;
    suback_seen_ = false;
    subacks_ = 0;
    limits_sig_ = 0;
    published_ = false;
    if (sendRaw(mqttlite::encodeConnect(buf, sizeof buf, o))) connect_sent_ = true;
}

void onConnack(int rc) {
    connack_seen_ = true;
    if (rc != 0) {
        // 4 = bad credentials, 5 = not authorised. Retrying will not fix it,
        // but the reconnect loop will keep trying at a gentle pace regardless.
        APPLOG("mqtt: broker refused connection, code %d", rc);
        ws.disconnect();
        return;
    }
    connected_ = true;
    last_state_ok_ = last_rx_ = millis();
    evlog::add(evlog::MqttUp);
    APPLOG("mqtt: connected");
    publishStr(status_topic, "online", true);
    sendRaw(mqttlite::encodeSubscribe(buf, sizeof buf, kSubscribePacketId, mode_set_topic));
    sendRaw(mqttlite::encodeSubscribe(buf, sizeof buf, kSubscribeLimitsPacketId, limits_set_topic));
    sendRaw(mqttlite::encodeSubscribe(buf, sizeof buf, kSubscribeProfilePacketId, profile_set_topic));
    discovery_next_ = 0;
    discovery_pending_ = true;
    last_state_ = 0; // publish state right away
}

void onEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
    case WStype_CONNECTED:
        APPLOG("mqtt: websocket up, sending CONNECT");
        sendConnect();
        break;
    case WStype_DISCONNECTED:
        if (connect_sent_ && !connected_ && !connack_seen_) {
            // Brokers often close right after refusing, and the CONNACK with the
            // reason may never reach us. Say what that usually means.
            APPLOG("mqtt: closed before CONNACK, likely bad username/password or ACL");
        } else if (connected_ || connect_sent_) {
            APPLOG("mqtt: disconnected");
        }
        if (connected_) evlog::add(evlog::MqttDown);
        connected_ = false;
        connect_sent_ = false;
        break;
    case WStype_BIN: {
        last_rx_ = millis();
        // A frame may carry several packets: the SUBACK and the retained command
        // it releases often arrive together. PINGRESP and friends are ignored.
        size_t off = 0;
        while (off < length) {
            const uint8_t* p = payload + off;
            const size_t n = mqttlite::packetLength(p, length - off);
            if (n == 0) break;
            off += n;

            switch (mqttlite::packetType(p)) {
            case 2: // CONNACK
                if (!connected_) {
                    const int rc = mqttlite::parseConnack(p, n);
                    if (rc >= 0) onConnack(rc);
                }
                break;
            case 9: // SUBACK
                onSuback(mqttlite::parseSuback(p, n));
                break;
            case 3: { // PUBLISH
                mqttlite::Publish pub;
                if (mqttlite::parsePublish(p, n, pub)) handlePublish(pub);
                break;
            }
            default:
                break;
            }
        }
        break;
    }
    default:
        break;
    }
}

// Cell entities wait for the BMS to say how many cells there are. A BMS that
// is not answering must not hold things up forever; the fixed entities and the
// select are already out, and the cells will follow once it answers.
bool discoveryDone() {
    return !discovery_pending_ ||
           (discovery_next_ > kEntityCount + 1 && bms->data().cell_count == 0 &&
            millis() >= kFirstDataWaitMs);
}

} // namespace

void begin(DalyBms* b) {
    bms = b;
    snprintf(status_topic, sizeof status_topic, "%s/status", MQTT_BASE_TOPIC);
    snprintf(state_topic, sizeof state_topic, "%s/state", MQTT_BASE_TOPIC);
    snprintf(mode_set_topic, sizeof mode_set_topic, "%s/mode/set", MQTT_BASE_TOPIC);
    snprintf(limits_topic, sizeof limits_topic, "%s/limits", MQTT_BASE_TOPIC);
    snprintf(limits_set_topic, sizeof limits_set_topic, "%s/limits/set", MQTT_BASE_TOPIC);
    snprintf(limits_result_topic, sizeof limits_result_topic, "%s/limits/result", MQTT_BASE_TOPIC);
    snprintf(profile_set_topic, sizeof profile_set_topic, "%s/profile/set", MQTT_BASE_TOPIC);
    ws.onEvent(onEvent);
    ws.setReconnectInterval(kReconnectMs);
    // The server itself is chosen by selectServer() once we know which network
    // we are on.
}

void selectServer(bool external) {
#ifndef MQTT_HOST_EXT
    external = false;
#endif
    if (server_chosen_ && external == server_external_) return;
    if (server_chosen_) {
        ws.disconnect();
        connected_ = false;
        connect_sent_ = false;
    }
    server_chosen_ = true;
    server_external_ = external;

    const char* host = MQTT_HOST;
    uint16_t port = MQTT_PORT;
    const char* fp_text = MQTT_TLS_FINGERPRINT;
#ifdef MQTT_HOST_EXT
    if (external) {
        host = MQTT_HOST_EXT;
        port = MQTT_PORT_EXT;
#ifdef MQTT_TLS_FINGERPRINT_EXT
        fp_text = MQTT_TLS_FINGERPRINT_EXT;
#endif
    }
#endif

    server_host_ = host;

#if MQTT_TLS
    // An empty fingerprint means "encrypt but do not verify the server".
    // BearSSL wants the SHA-1 fingerprint as 20 raw bytes. The pointer must stay
    // valid for the life of the connection, hence static.
    static uint8_t fingerprint[20];
    const bool pinned = parseFingerprint(fp_text, fingerprint);
    if (!pinned && strlen(fp_text) > 0) {
        // A typo in a pin must not quietly turn into "trust anyone": that is
        // only for an empty fingerprint, which is a deliberate choice. Leave the
        // connection unstarted until the config is fixed.
        APPLOG("mqtt: fingerprint is not 40 hex digits, refusing to connect");
        config_blocked_ = true;
        return;
    }
    config_blocked_ = false;
    ws.beginSSL(host, port, MQTT_PATH, pinned ? fingerprint : nullptr, "mqtt");
#else
    (void)fp_text;
    ws.begin(host, port, MQTT_PATH, "mqtt");
#endif
    APPLOG("mqtt: %s server %s://%s:%u%s", external ? "external" : "internal",
                 MQTT_TLS ? "wss" : "ws", host, static_cast<unsigned>(port), MQTT_PATH);
}

void loop() {
    // Nothing to talk over until WiFi is up; the library reconnects by itself.
    if (WiFi.status() != WL_CONNECTED || !server_chosen_ || config_blocked_) return;
    ws.loop();
    if (!connected_) return;

    stepDiscovery();
    sampleVoltage();

    // The guard state is the one thing worth sending the moment it changes.
    const voltguard::State g = voltguard::status().state;
    const bool guard_changed = g != last_guard_;
    last_guard_ = g;

    // A profile change is read back from the BMS some time after the command; the new
    // profile must reach Home Assistant before a sleeping mode goes back to sleep.
    const char* profile = settings::currentProfile(bms->data());
    const bool profile_changed = strcmp(profile, last_profile_) != 0;
    last_profile_ = profile;

    // Same for "BMS data fresh": it going stale must reach Home Assistant now, not
    // at the next periodic report.
    const bool bms_ok = bms->valid();
    const bool bms_ok_changed = bms_ok != last_bms_ok_;
    last_bms_ok_ = bms_ok;

    const powermode::Settings& s = powermode::settings();
    // Wait for a whole first sweep, not just 0x90: a sleeping mode sends one report
    // per wake, and it must not go out with the cell, temperature and limit readings
    // still missing.
    const bool first_ready = (bms_ok && bms->complete()) || millis() >= kFirstDataWaitMs;
    // The periodic report also waits until every reading has been seen once (or the
    // BMS has had long enough); a guard change goes out as soon as first_ready.
    // The limits (0x59/0x5A) come after the live commands in a sweep; without them the
    // pack profile reads "unknown", and a sleeping mode retains that until the next wake.
    const bool limits_seen = bms->data().pack_low_l1_dv != 0;
    const bool data_ready =
        bms_ok ? (first_ready && ((bms->allLiveSeen() && limits_seen) || millis() >= kLiveDataGiveUpMs))
               : first_ready;
    // A periodic report waits for a sweep that has just finished, so a slow poll
    // (normal mode polls once per report) sends fresh numbers instead of the last
    // sweep's. Unavailable data is not held back: that is news at once.
    // Fresh means the sweep has finished too, not only that 0x90 came in: the other
    // readings of a sweep still in progress are from the one before.
    const bool fresh = bms->secondsSincePack() <= kFreshForReportS && !bms->busy();
    // The first report of a connection counts as due too: after a reconnect the
    // newest reading may be minutes old.
    const bool overdue = last_state_ == 0 || millis() - last_state_ >= s.publish_ms;
    // A report that falls due between sweeps asks for one instead of waiting out
    // another whole poll interval.
    if (overdue && bms_ok && !fresh && !bms->busy() &&
        (report_sweep_at_ == 0 || millis() - report_sweep_at_ >= kReportSweepGapMs)) {
        report_sweep_at_ = millis() | 1; // never 0, which means "not asked yet"
        bms->requestSweep();
    }
    const bool due = overdue && (fresh || !bms_ok);
    const bool retry_due = retry_pending_ && static_cast<int32_t>(millis() - retry_at_) >= 0;
    // (A disabled guard differs from the initial snapshot at boot; that is not news.)
    if (guard_changed && g != voltguard::State::Disabled) urgent_owed_ = true;
    if (profile_changed || bms_ok_changed) change_owed_ = true;
    const bool want = (first_ready && urgent_owed_) || (data_ready && (change_owed_ || due || retry_due));
    // After a failed report wait out the 5 s, whatever else asks for one.
    if (want && !(retry_pending_ && !retry_due)) {
        last_state_ = millis() | 1; // never 0, which means "publish now"
        retry_pending_ = false;
        if (publishState()) {
            urgent_owed_ = false;
            change_owed_ = false;
        } else {
            // Out of heap or send buffer: try again in 5 s, not a whole interval.
            APPLOG("mqtt: state not published, retrying");
            evlog::add(evlog::PubFail);
            retry_pending_ = true;
            retry_at_ = millis() + 5000;
        }
    }

    publishLimits();
    reportWriteOutcome();

    // A mode switch does not reconnect, so never ping slower than half of what this
    // connection announced, whatever the mode now says.
    // (A connection that announced no keepalive has no such limit.)
    uint32_t ping_ms = s.ping_ms;
    if (ping_ms != 0 && keepalive_s_ != 0) ping_ms = min<uint32_t>(ping_ms, keepalive_s_ * 500UL);
    if (ping_ms != 0 && millis() - last_ping_ >= ping_ms) {
        last_ping_ = millis();
        sendRaw(mqttlite::encodePingReq(buf, sizeof buf));
    }
}

void publishNow() { last_state_ = 0; }

bool sessionComplete() {
    // A limits change still being written to the BMS must not be cut off by sleep.
    return settled() && !bms->writePending() && millis() - suback_at_ >= kRetainedGraceMs;
}

bool writePending() { return bms != nullptr && bms->writePending(); }

bool reportPending() {
    return connected_ && (voltguard::status().state != last_guard_ || retry_pending_ || change_owed_ ||
                          strcmp(settings::currentProfile(bms->data()), last_profile_) != 0);
}

bool settled() { return connected_ && published_ && discoveryDone() && suback_seen_; }

void goodbye() {
    if (!connected_) return;
    // A clean DISCONNECT keeps the broker from publishing the "offline" will, so
    // Home Assistant goes on showing the last values while the bridge sleeps.
    sendRaw(mqttlite::encodeDisconnect(buf, sizeof buf));
    // The write has to leave the radio before the CPU stops.
    for (int i = 0; i < 5; i++) {
        ws.loop();
        delay(40);
    }
    ws.disconnect();
    connected_ = false;
}

bool connected() { return connected_; }

uint32_t msSilent() {
    if (!server_chosen_ || config_blocked_) return 0; // nothing is expected to happen
    const uint32_t now = millis();
    uint32_t age = now - last_state_ok_;
    // With pings on, the broker answers one at least every ping interval, so a long
    // quiet from it means the link is dead even if our sends still "succeed".
    if (connected_ && powermode::settings().ping_ms != 0) age = max(age, now - last_rx_);
    return age;
}

const char* serverName() {
    if (!server_chosen_) return "none";
    return server_external_ ? "external" : "internal";
}

const char* serverHost() { return server_host_; }

} // namespace ha_mqtt
