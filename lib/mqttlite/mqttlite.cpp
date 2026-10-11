#include "mqttlite.h"

#include <string.h>

namespace mqttlite {
namespace {

constexpr uint8_t TYPE_CONNECT = 0x10;
constexpr uint8_t TYPE_PUBLISH = 0x30;
constexpr uint8_t TYPE_PINGREQ = 0xC0;
constexpr uint8_t TYPE_DISCONNECT = 0xE0;
constexpr uint8_t TYPE_CONNACK = 0x20;
constexpr uint8_t TYPE_SUBSCRIBE = 0x82; // the 0b0010 flags are mandatory
constexpr uint8_t TYPE_SUBACK = 0x90;

// Bounds-checked writer; any overflow latches `ok` false and the caller returns 0.
struct Writer {
    uint8_t* buf;
    size_t cap;
    size_t pos = 0;
    bool ok = true;

    void u8(uint8_t v) {
        if (pos < cap) buf[pos] = v;
        else ok = false;
        pos++;
    }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v >> 8));
        u8(static_cast<uint8_t>(v & 0xFF));
    }
    void bytes(const uint8_t* p, size_t n) {
        if (pos + n <= cap) {
            // memmove: the payload may already sit inside `buf`, further along than
            // where it is going (see ha_mqtt.cpp publishState).
            if (n > 0) memmove(buf + pos, p, n);
        } else {
            ok = false;
        }
        pos += n;
    }
    // MQTT UTF-8 string: 2-byte length prefix.
    void str(const char* s) {
        const size_t n = strlen(s);
        if (n > 0xFFFF) {
            ok = false;
            return;
        }
        u16(static_cast<uint16_t>(n));
        bytes(reinterpret_cast<const uint8_t*>(s), n);
    }
};

size_t lenOfStr(const char* s) { return 2 + strlen(s); }

// Fixed header: type byte + variable-length "remaining length" (max 4 bytes).
bool header(Writer& w, uint8_t first_byte, size_t remaining) {
    if (remaining > 268435455UL) {
        w.ok = false;
        return false;
    }
    w.u8(first_byte);
    do {
        uint8_t b = remaining % 128;
        remaining /= 128;
        if (remaining > 0) b |= 0x80;
        w.u8(b);
    } while (remaining > 0);
    return true;
}

} // namespace

size_t encodeConnect(uint8_t* buf, size_t cap, const ConnectOptions& o) {
    const bool has_will = o.will_topic != nullptr;
    const bool has_user = o.username != nullptr;
    const bool has_pass = has_user && o.password != nullptr;

    uint8_t flags = 0;
    if (o.clean_session) flags |= 0x02;
    if (has_will) {
        flags |= 0x04;
        if (o.will_retain) flags |= 0x20;
    }
    if (has_pass) flags |= 0x40;
    if (has_user) flags |= 0x80;

    const char* will_payload = o.will_payload ? o.will_payload : "";
    size_t remaining = lenOfStr("MQTT") + 1 /*level*/ + 1 /*flags*/ + 2 /*keepalive*/;
    remaining += lenOfStr(o.client_id);
    if (has_will) remaining += lenOfStr(o.will_topic) + lenOfStr(will_payload);
    if (has_user) remaining += lenOfStr(o.username);
    if (has_pass) remaining += lenOfStr(o.password);

    Writer w{buf, cap};
    if (!header(w, TYPE_CONNECT, remaining)) return 0;
    w.str("MQTT");
    w.u8(4); // protocol level 4 = MQTT 3.1.1
    w.u8(flags);
    w.u16(o.keepalive_s);
    w.str(o.client_id);
    if (has_will) {
        w.str(o.will_topic);
        w.str(will_payload); // length-prefixed binary on the wire; text here
    }
    if (has_user) w.str(o.username);
    if (has_pass) w.str(o.password);
    return w.ok ? w.pos : 0;
}

size_t encodePublish(uint8_t* buf, size_t cap, const char* topic, const uint8_t* payload,
                     size_t payload_len, bool retain) {
    const size_t remaining = lenOfStr(topic) + payload_len; // QoS 0: no packet id
    Writer w{buf, cap};
    if (!header(w, TYPE_PUBLISH | (retain ? 0x01 : 0x00), remaining)) return 0;
    w.str(topic);
    w.bytes(payload, payload_len);
    return w.ok ? w.pos : 0;
}

size_t encodePingReq(uint8_t* buf, size_t cap) {
    Writer w{buf, cap};
    header(w, TYPE_PINGREQ, 0);
    return w.ok ? w.pos : 0;
}

size_t encodeDisconnect(uint8_t* buf, size_t cap) {
    Writer w{buf, cap};
    header(w, TYPE_DISCONNECT, 0);
    return w.ok ? w.pos : 0;
}

size_t encodeSubscribe(uint8_t* buf, size_t cap, uint16_t packet_id, const char* topic) {
    const size_t remaining = 2 /*packet id*/ + lenOfStr(topic) + 1 /*requested QoS*/;
    Writer w{buf, cap};
    if (!header(w, TYPE_SUBSCRIBE, remaining)) return 0;
    w.u16(packet_id);
    w.str(topic);
    w.u8(0);
    return w.ok ? w.pos : 0;
}

namespace {

// Decodes the remaining-length field. `hdr` is how many bytes the fixed header
// takes (1 + the length bytes). False if incomplete or longer than 4 bytes.
bool readRemaining(const uint8_t* buf, size_t len, size_t& remaining, size_t& hdr) {
    remaining = 0;
    size_t mult = 1;
    for (size_t i = 1; i <= 4; i++) {
        if (i >= len) return false;
        remaining += (buf[i] & 0x7F) * mult;
        if ((buf[i] & 0x80) == 0) {
            hdr = i + 1;
            return true;
        }
        mult *= 128;
    }
    return false;
}

} // namespace

size_t packetLength(const uint8_t* buf, size_t len) {
    size_t remaining, hdr;
    if (len < 2 || !readRemaining(buf, len, remaining, hdr)) return 0;
    return hdr + remaining <= len ? hdr + remaining : 0;
}

bool parsePublish(const uint8_t* buf, size_t len, Publish& out) {
    if (len < 2 || (buf[0] & 0xF0) != TYPE_PUBLISH) return false;
    size_t remaining, hdr;
    if (!readRemaining(buf, len, remaining, hdr) || hdr + remaining != len) return false;
    if (remaining < 2) return false;

    size_t pos = hdr;
    const size_t topic_len = (static_cast<size_t>(buf[pos]) << 8) | buf[pos + 1];
    pos += 2;
    if (pos + topic_len > len) return false;
    out.topic = reinterpret_cast<const char*>(buf + pos);
    out.topic_len = topic_len;
    pos += topic_len;

    const uint8_t qos = (buf[0] >> 1) & 0x03;
    if (qos == 3) return false;
    if (qos > 0) pos += 2; // packet id; we never ack, and never ask for QoS > 0
    if (pos > len) return false;

    out.payload = buf + pos;
    out.payload_len = len - pos;
    return true;
}

int parseSuback(const uint8_t* buf, size_t len) {
    // type, remaining length, packet id (2), one return code per filter
    if (len < 5 || buf[0] != TYPE_SUBACK || buf[1] < 3) return -1;
    return buf[4];
}

int parseConnack(const uint8_t* buf, size_t len) {
    if (len < 4 || buf[0] != TYPE_CONNACK || buf[1] != 2) return -1;
    return buf[3];
}

} // namespace mqttlite
