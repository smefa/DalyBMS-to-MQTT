// Just enough MQTT 3.1.1 to publish and to take one command back: CONNECT,
// PUBLISH (QoS 0), SUBSCRIBE (QoS 0), PINGREQ, DISCONNECT, and to read CONNACK,
// SUBACK and incoming PUBLISH. No QoS 1/2.
//
// Pure byte-shuffling with no Arduino dependency, so it unit-tests on the host.
// Each encoder returns the packet length, or 0 if it does not fit in `cap`.
// The transport (here: binary WebSocket frames) is the caller's concern.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mqttlite {

struct ConnectOptions {
    const char* client_id = "";
    const char* username = nullptr; // optional
    const char* password = nullptr; // optional, only sent together with username
    uint16_t keepalive_s = 30;
    bool clean_session = true;

    // Last will, published by the broker if we vanish without DISCONNECT.
    const char* will_topic = nullptr; // optional
    const char* will_payload = nullptr;
    bool will_retain = false;
};

size_t encodeConnect(uint8_t* buf, size_t cap, const ConnectOptions& o);

size_t encodePublish(uint8_t* buf, size_t cap, const char* topic, const uint8_t* payload,
                     size_t payload_len, bool retain);

size_t encodePingReq(uint8_t* buf, size_t cap);
size_t encodeDisconnect(uint8_t* buf, size_t cap);

// One topic filter, requested at QoS 0.
size_t encodeSubscribe(uint8_t* buf, size_t cap, uint16_t packet_id, const char* topic);

// --- reading what the broker sends ------------------------------------------
//
// A WebSocket frame usually holds one MQTT packet but may hold several (a SUBACK
// and the retained message it triggers, say). Walk them with packetLength().

// Bytes in the first packet of `buf`, or 0 if it is not all there yet (or the
// length field is malformed).
size_t packetLength(const uint8_t* buf, size_t len);

// Packet type, the high nibble of the first byte: 2 CONNACK, 3 PUBLISH, 9 SUBACK...
inline uint8_t packetType(const uint8_t* buf) { return buf[0] >> 4; }

struct Publish {
    const char* topic;      // points into the packet, NOT NUL-terminated
    size_t topic_len;
    const uint8_t* payload; // points into the packet
    size_t payload_len;
};

// `buf` must hold exactly one complete packet (see packetLength). False if it is
// not a PUBLISH or is malformed.
bool parsePublish(const uint8_t* buf, size_t len, Publish& out);

// First return code of a SUBACK: 0..2 is the granted QoS, 0x80 is refused.
// -1 if `buf` is not a complete SUBACK.
int parseSuback(const uint8_t* buf, size_t len);

// Returns the CONNACK return code (0 = accepted, 1..5 = refused), or -1 if
// `buf` does not start with a complete CONNACK.
int parseConnack(const uint8_t* buf, size_t len);

} // namespace mqttlite
