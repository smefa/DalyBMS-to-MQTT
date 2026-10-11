#include "ha_mqtt.h"

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WebSocketsClient.h>

#include "applog.h"
#include "config.h"
#include "mqttlite.h"
#include "powermode.h"
#include "voltguard.h"

namespace ha_mqtt {
namespace {

// Pings come from powermode::settings().ping_ms and must stay well inside the
// 1.5x keepalive window the broker enforces.
constexpr uint16_t kKeepaliveS = 60;
constexpr uint32_t kReconnectMs = 5000;
constexpr size_t kBufSize = 1200;        // largest packet: 32-cell state or a discovery config

// Do not publish the very first state until the BMS has answered, or give up
// waiting after this long since boot: a BMS that never answers is itself news.
constexpr uint32_t kFirstDataWaitMs = 15000;
// A retained command arrives right behind the SUBACK. A storage session stays up
// this long after it to be sure it has seen it.
constexpr uint32_t kRetainedGraceMs = 1500;
constexpr uint16_t kSubscribePacketId = 1;

const DalyBms* bms = nullptr;
WebSocketsClient ws;

uint8_t buf[kBufSize];
char status_topic[64];
char state_topic[64];
char mode_set_topic[64];

bool connected_ = false;
bool connect_sent_ = false;
bool connack_seen_ = false;
bool suback_seen_ = false;     // the broker answered our SUBSCRIBE, either way
bool published_ = false;       // a state document has gone out on this connection
uint32_t suback_at_ = 0;
bool server_chosen_ = false;
bool server_external_ = false;
const char* server_host_ = "";
uint32_t last_tx_ = 0;
uint32_t last_state_ = 0;

// Discovery is sent one config per loop() pass so the radio and the BMS driver
// are never starved by a burst of 40 publishes.
uint8_t discovery_next_ = 0;
bool discovery_pending_ = false;

voltguard::State last_guard_ = voltguard::State::Normal;

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
    {"sensor", "uptime_s", "Uptime", "s", "duration", nullptr, true},
};
constexpr uint8_t kEntityCount = sizeof(kEntities) / sizeof(kEntities[0]);

// ---- Sending ---------------------------------------------------------------

bool sendRaw(size_t n) {
    if (n == 0) return false;
    last_tx_ = millis();
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

// In storage mode the bridge is silent for a day at a time, and a clean
// disconnect leaves "online" retained, so a bridge that died asleep would look
// healthy forever. Expire the values instead: twice the storage period leaves
// room for one missed report.
uint32_t expireAfterS() { return powermode::storageIntervalS() * 2; }

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

bool publishEntityDiscovery(const Entity& e) {
    JsonDocument doc;
    baseConfig(doc, e.id, e.name);
    char tpl[48];
    snprintf(tpl, sizeof tpl, "{{ value_json.%s }}", e.id);
    doc["val_tpl"] = tpl;
    if (e.unit) doc["unit_of_meas"] = e.unit;
    if (e.dev_class) doc["dev_cla"] = e.dev_class;
    if (e.state_class) doc["stat_cla"] = e.state_class;
    if (e.diagnostic) doc["ent_cat"] = "diagnostic";
    doc["exp_aft"] = expireAfterS();

    char topic[128];
    discoveryTopic(topic, sizeof topic, e.component, e.id);
    char out[640];
    const size_t n = serializeJson(doc, out, sizeof out);
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
    const size_t n = serializeJson(doc, out, sizeof out);
    return publish(topic, out, n, true);
}

bool publishCellDiscovery(uint8_t cell) {
    char id[12], name[16];
    snprintf(id, sizeof id, "cell_%u", cell);
    snprintf(name, sizeof name, "Cell %u", cell);

    JsonDocument doc;
    baseConfig(doc, id, name);
    char tpl[48];
    snprintf(tpl, sizeof tpl, "{{ value_json.%s }}", id);
    doc["val_tpl"] = tpl;
    doc["unit_of_meas"] = "mV";
    doc["dev_cla"] = "voltage";
    doc["stat_cla"] = "measurement";
    doc["ent_cat"] = "diagnostic";
    doc["exp_aft"] = expireAfterS();

    char topic[128];
    discoveryTopic(topic, sizeof topic, "sensor", id);
    char out[640];
    const size_t n = serializeJson(doc, out, sizeof out);
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

    const uint8_t cells = bms->data().cell_count;
    if (cells == 0) return; // keep waiting for the BMS to tell us
    const uint8_t cell = discovery_next_ - kEntityCount; // 1-based: slot kEntityCount is the select
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

void publishState() {
    const daly::BmsData& d = bms->data();
    const voltguard::Status& g = voltguard::status();

    const float volts = d.pack_voltage_dv / 10.0f;
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
    doc["stale_s"] = bms->secondsSinceGoodFrame();
    doc["guard"] = voltguard::stateName();
    doc["guard_low_s"] = g.low_for_s;
    doc["rssi"] = WiFi.RSSI();
    doc["uptime_s"] = millis() / 1000;
    doc["mode"] = powermode::modeName();
    for (uint8_t i = 0; i < d.cells_reported; i++) {
        char key[12];
        snprintf(key, sizeof key, "cell_%u", i + 1);
        doc[key] = d.cell_mv[i];
    }

    // Serialised straight into the send buffer, behind room for the PUBLISH header,
    // rather than into a buffer of its own: with TLS up the heap is a few kB and a
    // second 1 kB buffer is a lot. (The ESP8266 stack is only 4 kB, so not on that.)
    constexpr size_t kHeaderRoom = 128;
    char* out = reinterpret_cast<char*>(buf) + kHeaderRoom;
    const size_t n = serializeJson(doc, out, kBufSize - kHeaderRoom);
    // Retained in storage mode only: the next report is a day away, and without
    // it a restarted Home Assistant shows every entity as unknown until then.
    if (publish(state_topic, out, n, powermode::mode() == powermode::Mode::Storage)) {
        published_ = true;
    }
}

// ---- Incoming --------------------------------------------------------------

void handlePublish(const mqttlite::Publish& p) {
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
    suback_seen_ = true;
    suback_at_ = millis();
    if (rc < 0 || rc >= 0x80) {
        APPLOG("mqtt: broker refused subscribe to %s, mode cannot be changed over MQTT "
                     "(does the broker user have read access to it?)",
                     mode_set_topic);
    }
}

// ---- Connection ------------------------------------------------------------

void sendConnect() {
    mqttlite::ConnectOptions o;
    o.client_id = DEVICE_HOSTNAME;
    o.keepalive_s = kKeepaliveS;
    o.will_topic = status_topic;
    o.will_payload = "offline";
    o.will_retain = true;
#ifdef MQTT_USER
    o.username = MQTT_USER;
    o.password = MQTT_PASSWORD;
#endif
    connack_seen_ = false;
    suback_seen_ = false;
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
    APPLOG("mqtt: connected");
    publishStr(status_topic, "online", true);
    sendRaw(mqttlite::encodeSubscribe(buf, sizeof buf, kSubscribePacketId, mode_set_topic));
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
        connected_ = false;
        connect_sent_ = false;
        break;
    case WStype_BIN: {
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
           (discovery_next_ > kEntityCount && bms->data().cell_count == 0 &&
            millis() >= kFirstDataWaitMs);
}

} // namespace

void begin(const DalyBms* b) {
    bms = b;
    snprintf(status_topic, sizeof status_topic, "%s/status", MQTT_BASE_TOPIC);
    snprintf(state_topic, sizeof state_topic, "%s/state", MQTT_BASE_TOPIC);
    snprintf(mode_set_topic, sizeof mode_set_topic, "%s/mode/set", MQTT_BASE_TOPIC);
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
        APPLOG("mqtt: fingerprint is not 40 hex digits, NOT verifying server");
    }
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
    if (WiFi.status() != WL_CONNECTED || !server_chosen_) return;
    ws.loop();
    if (!connected_) return;

    stepDiscovery();

    // The guard state is the one thing worth sending the moment it changes.
    const voltguard::State g = voltguard::status().state;
    const bool guard_changed = g != last_guard_;
    last_guard_ = g;

    const powermode::Settings& s = powermode::settings();
    const bool data_ready = bms->valid() || millis() >= kFirstDataWaitMs;
    if (data_ready &&
        (guard_changed || last_state_ == 0 || millis() - last_state_ >= s.publish_ms)) {
        last_state_ = millis() | 1; // never 0, which means "publish now"
        publishState();
    }

    if (millis() - last_tx_ >= s.ping_ms) {
        sendRaw(mqttlite::encodePingReq(buf, sizeof buf));
    }
}

void publishNow() { last_state_ = 0; }

bool sessionComplete() {
    return settled() && millis() - suback_at_ >= kRetainedGraceMs;
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

const char* serverName() {
    if (!server_chosen_) return "none";
    return server_external_ ? "external" : "internal";
}

const char* serverHost() { return server_host_; }

} // namespace ha_mqtt
