#include <string.h>
#include <unity.h>

#include "mqttlite.h"

using namespace mqttlite;

void setUp() {}
void tearDown() {}

void test_pingreq_disconnect() {
    uint8_t b[8];
    TEST_ASSERT_EQUAL(2, encodePingReq(b, sizeof b));
    TEST_ASSERT_EQUAL_HEX8(0xC0, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, b[1]);
    TEST_ASSERT_EQUAL(2, encodeDisconnect(b, sizeof b));
    TEST_ASSERT_EQUAL_HEX8(0xE0, b[0]);
}

void test_publish_basic() {
    uint8_t b[32];
    const uint8_t p[] = {'h', 'i'};
    const size_t n = encodePublish(b, sizeof b, "a/b", p, 2, false);
    const uint8_t expect[] = {0x30, 0x07, 0x00, 0x03, 'a', '/', 'b', 'h', 'i'};
    TEST_ASSERT_EQUAL(sizeof expect, n);
    TEST_ASSERT_EQUAL_MEMORY(expect, b, n);
}

void test_publish_payload_already_inside_buffer() {
    // The caller builds the payload at buf+32 and the header lands in front of it,
    // overlapping the payload's old position once it is moved down.
    uint8_t b[128];
    memset(b, 0, sizeof b);
    const char* text = "0123456789abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJ"; // 56 bytes
    memcpy(b + 32, text, 56);
    const size_t n = encodePublish(b, sizeof b, "a/b", b + 32, 56, false);
    TEST_ASSERT_EQUAL(1 + 1 + 2 + 3 + 56, n);
    TEST_ASSERT_EQUAL_MEMORY(text, b + 7, 56);
}

void test_publish_retain_flag() {
    uint8_t b[16];
    encodePublish(b, sizeof b, "t", nullptr, 0, true);
    TEST_ASSERT_EQUAL_HEX8(0x31, b[0]);
}

void test_publish_long_payload_uses_two_length_bytes() {
    static uint8_t payload[300];
    static uint8_t b[400];
    memset(payload, 'x', sizeof payload);
    const size_t n = encodePublish(b, sizeof b, "t", payload, sizeof payload, false);
    // remaining = 2 + 1 + 300 = 303 -> 0xAF 0x02
    TEST_ASSERT_EQUAL(1 + 2 + 303, n);
    TEST_ASSERT_EQUAL_HEX8(0xAF, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02, b[2]);
}

void test_publish_overflow_returns_zero() {
    uint8_t b[5];
    const uint8_t p[] = {1, 2, 3};
    TEST_ASSERT_EQUAL(0, encodePublish(b, sizeof b, "topic", p, 3, false));
}

void test_connect_minimal() {
    uint8_t b[64];
    ConnectOptions o;
    o.client_id = "id";
    o.keepalive_s = 30;
    const size_t n = encodeConnect(b, sizeof b, o);
    const uint8_t expect[] = {0x10, 0x0E, 0x00, 0x04, 'M',  'Q', 'T', 'T',
                              0x04, 0x02, 0x00, 0x1E, 0x00, 0x02, 'i', 'd'};
    TEST_ASSERT_EQUAL(sizeof expect, n);
    TEST_ASSERT_EQUAL_MEMORY(expect, b, n);
}

void test_connect_full_flags() {
    uint8_t b[128];
    ConnectOptions o;
    o.client_id = "c";
    o.username = "u";
    o.password = "p";
    o.will_topic = "w";
    o.will_payload = "off";
    o.will_retain = true;
    const size_t n = encodeConnect(b, sizeof b, o);
    TEST_ASSERT_TRUE(n > 0);
    // clean(0x02) | will(0x04) | will retain(0x20) | pass(0x40) | user(0x80)
    TEST_ASSERT_EQUAL_HEX8(0xE6, b[9]);
    TEST_ASSERT_EQUAL(static_cast<size_t>(b[1]) + 2, n);
}

void test_connect_password_without_username_is_ignored() {
    uint8_t b[64];
    ConnectOptions o;
    o.client_id = "c";
    o.password = "p";
    encodeConnect(b, sizeof b, o);
    TEST_ASSERT_EQUAL_HEX8(0x02, b[9]);
}

void test_connect_overflow_returns_zero() {
    uint8_t b[10];
    ConnectOptions o;
    o.client_id = "client";
    TEST_ASSERT_EQUAL(0, encodeConnect(b, sizeof b, o));
}

void test_connack() {
    const uint8_t ok[] = {0x20, 0x02, 0x00, 0x00};
    const uint8_t refused[] = {0x20, 0x02, 0x00, 0x05};
    const uint8_t bad[] = {0x30, 0x02, 0x00, 0x00};
    TEST_ASSERT_EQUAL(0, parseConnack(ok, 4));
    TEST_ASSERT_EQUAL(5, parseConnack(refused, 4));
    TEST_ASSERT_EQUAL(-1, parseConnack(bad, 4));
    TEST_ASSERT_EQUAL(-1, parseConnack(ok, 3));
}

void test_subscribe() {
    uint8_t b[32];
    const size_t n = encodeSubscribe(b, sizeof b, 1, "a/b");
    const uint8_t expect[] = {0x82, 0x08, 0x00, 0x01, 0x00, 0x03, 'a', '/', 'b', 0x00};
    TEST_ASSERT_EQUAL(sizeof expect, n);
    TEST_ASSERT_EQUAL_MEMORY(expect, b, n);
    TEST_ASSERT_EQUAL(0, encodeSubscribe(b, 6, 1, "a/b"));
}

void test_suback() {
    const uint8_t granted[] = {0x90, 0x03, 0x00, 0x01, 0x00};
    const uint8_t refused[] = {0x90, 0x03, 0x00, 0x01, 0x80};
    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL(0, parseSuback(granted, sizeof granted));
    TEST_ASSERT_EQUAL(0x80, parseSuback(refused, sizeof refused));
    TEST_ASSERT_EQUAL(-1, parseSuback(granted, 4));
    TEST_ASSERT_EQUAL(-1, parseSuback(connack, sizeof connack));
}

void test_parse_publish_round_trip() {
    uint8_t b[64];
    const uint8_t p[] = {'h', 'i', 'g', 'h'};
    const size_t n = encodePublish(b, sizeof b, "dalybms/mode/set", p, sizeof p, true);
    TEST_ASSERT_EQUAL(n, packetLength(b, n));

    Publish pub;
    TEST_ASSERT_TRUE(parsePublish(b, n, pub));
    TEST_ASSERT_EQUAL(16, pub.topic_len);
    TEST_ASSERT_EQUAL_MEMORY("dalybms/mode/set", pub.topic, 16);
    TEST_ASSERT_EQUAL(4, pub.payload_len);
    TEST_ASSERT_EQUAL_MEMORY("high", pub.payload, 4);
}

void test_parse_publish_empty_payload() {
    uint8_t b[16];
    const size_t n = encodePublish(b, sizeof b, "t", nullptr, 0, false);
    Publish pub;
    TEST_ASSERT_TRUE(parsePublish(b, n, pub));
    TEST_ASSERT_EQUAL(0, pub.payload_len);
}

void test_parse_publish_skips_packet_id_for_qos1() {
    // PUBLISH QoS 1: topic "t", packet id 0x0102, payload "x"
    const uint8_t b[] = {0x32, 0x06, 0x00, 0x01, 't', 0x01, 0x02, 'x'};
    Publish pub;
    TEST_ASSERT_TRUE(parsePublish(b, sizeof b, pub));
    TEST_ASSERT_EQUAL(1, pub.payload_len);
    TEST_ASSERT_EQUAL_HEX8('x', pub.payload[0]);
}

void test_parse_publish_rejects_malformed() {
    Publish pub;
    const uint8_t not_publish[] = {0x20, 0x02, 0x00, 0x00};
    TEST_ASSERT_FALSE(parsePublish(not_publish, sizeof not_publish, pub));
    // topic length claims 9 bytes but the packet ends first
    const uint8_t lying[] = {0x30, 0x04, 0x00, 0x09, 'a', 'b'};
    TEST_ASSERT_FALSE(parsePublish(lying, sizeof lying, pub));
    // more bytes handed in than the remaining-length field accounts for
    const uint8_t trailing[] = {0x30, 0x03, 0x00, 0x01, 't', 'x'};
    TEST_ASSERT_FALSE(parsePublish(trailing, sizeof trailing, pub));
    TEST_ASSERT_FALSE(parsePublish(trailing, 1, pub));
}

void test_packet_length_walks_concatenated_packets() {
    // SUBACK followed by a retained PUBLISH, as one WebSocket frame
    uint8_t b[64] = {0x90, 0x03, 0x00, 0x01, 0x00};
    const uint8_t p[] = {'n'};
    const size_t m = encodePublish(b + 5, sizeof b - 5, "t", p, 1, true);
    const size_t total = 5 + m;

    TEST_ASSERT_EQUAL(5, packetLength(b, total));
    TEST_ASSERT_EQUAL(9, packetType(b));
    TEST_ASSERT_EQUAL(m, packetLength(b + 5, total - 5));
    TEST_ASSERT_EQUAL(3, packetType(b + 5));
}

void test_packet_length_incomplete_or_bad() {
    const uint8_t half[] = {0x30, 0x05, 0x00};
    TEST_ASSERT_EQUAL(0, packetLength(half, sizeof half));
    TEST_ASSERT_EQUAL(0, packetLength(half, 1));
    const uint8_t endless[] = {0x30, 0x80, 0x80, 0x80, 0x80, 0x01};
    TEST_ASSERT_EQUAL(0, packetLength(endless, sizeof endless));
    uint8_t big[200] = {0x30, 0xAF, 0x02}; // remaining 303, only 197 bytes follow
    TEST_ASSERT_EQUAL(0, packetLength(big, sizeof big));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_pingreq_disconnect);
    RUN_TEST(test_publish_basic);
    RUN_TEST(test_publish_payload_already_inside_buffer);
    RUN_TEST(test_publish_retain_flag);
    RUN_TEST(test_publish_long_payload_uses_two_length_bytes);
    RUN_TEST(test_publish_overflow_returns_zero);
    RUN_TEST(test_connect_minimal);
    RUN_TEST(test_connect_full_flags);
    RUN_TEST(test_connect_password_without_username_is_ignored);
    RUN_TEST(test_connect_overflow_returns_zero);
    RUN_TEST(test_connack);
    RUN_TEST(test_subscribe);
    RUN_TEST(test_suback);
    RUN_TEST(test_parse_publish_round_trip);
    RUN_TEST(test_parse_publish_empty_payload);
    RUN_TEST(test_parse_publish_skips_packet_id_for_qos1);
    RUN_TEST(test_parse_publish_rejects_malformed);
    RUN_TEST(test_packet_length_walks_concatenated_packets);
    RUN_TEST(test_packet_length_incomplete_or_bad);
    return UNITY_END();
}
