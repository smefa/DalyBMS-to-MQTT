// Host-side tests for the DALY frame parser. No hardware required:
//   pio test -e native
#include <string.h>
#include <unity.h>

#include "daly_protocol.h"

using namespace daly;

// ---------------------------------------------------------------- helpers

static void makeFrame(uint8_t addr, uint8_t cmd, const uint8_t payload[8], uint8_t* out) {
    out[0] = START_BYTE;
    out[1] = addr;
    out[2] = cmd;
    out[3] = PAYLOAD_LEN;
    memcpy(out + 4, payload, 8);
    out[FRAME_LEN - 1] = checksum(out, FRAME_LEN - 1);
}

// Feeds a whole frame; returns true if the decoder emitted it.
static bool feedFrame(FrameDecoder& dec, const uint8_t* frame, BmsData& data) {
    bool got = false;
    for (uint8_t i = 0; i < FRAME_LEN; i++) {
        if (dec.feed(frame[i])) {
            applyFrame(dec.frame(), data);
            got = true;
        }
    }
    return got;
}

// ---------------------------------------------------------------- framing

void test_checksum_matches_real_capture(void) {
    // Real response captured in esphome/esphome#12445.
    const uint8_t rx[FRAME_LEN] = {0xA5, 0x02, 0x90, 0x08, 0x00, 0x82, 0x00,
                                   0x00, 0x75, 0x30, 0x01, 0xF4, 0x5B};
    TEST_ASSERT_EQUAL_HEX8(0x5B, checksum(rx, FRAME_LEN - 1));
}

void test_build_request(void) {
    uint8_t req[FRAME_LEN];
    buildRequest(CMD_PACK, req);

    const uint8_t expected[FRAME_LEN] = {0xA5, 0x40, 0x90, 0x08, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x7D};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, req, FRAME_LEN);
}

// The regression test this whole project hinges on. The BMS answers with its
// own physical address, not the 0x40 we sent. A decoder that matches on the
// address byte silently discards every valid response -- esphome#12445, closed
// "not planned", still unfixed upstream.
void test_response_address_byte_is_not_matched(void) {
    const uint8_t rx[FRAME_LEN] = {0xA5, 0x02, 0x90, 0x08, 0x00, 0x82, 0x00,
                                   0x00, 0x75, 0x30, 0x01, 0xF4, 0x5B};
    FrameDecoder dec;
    BmsData data;

    TEST_ASSERT_TRUE_MESSAGE(feedFrame(dec, rx, data),
                             "frame with address 0x02 must be accepted");
    TEST_ASSERT_EQUAL_UINT16(130, data.pack_voltage_dv); // 13.0 V
    TEST_ASSERT_EQUAL_INT16(0, data.pack_current_da);    // 0x7530 - 30000
    TEST_ASSERT_EQUAL_UINT16(500, data.soc_dpct);        // 50.0 %
}

void test_address_byte_01_also_accepted(void) {
    const uint8_t payload[8] = {0x00, 0x82, 0x00, 0x00, 0x75, 0x30, 0x01, 0xF4};
    uint8_t frame[FRAME_LEN];
    makeFrame(0x01, CMD_PACK, payload, frame);

    FrameDecoder dec;
    BmsData data;
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    TEST_ASSERT_EQUAL_UINT16(130, data.pack_voltage_dv);
}

void test_bad_checksum_rejected(void) {
    uint8_t rx[FRAME_LEN] = {0xA5, 0x01, 0x90, 0x08, 0x00, 0x82, 0x00,
                             0x00, 0x75, 0x30, 0x01, 0xF4, 0x5B};
    rx[5] ^= 0xFF; // corrupt a payload byte, leave the checksum stale

    FrameDecoder dec;
    BmsData data;
    TEST_ASSERT_FALSE(feedFrame(dec, rx, data));
    TEST_ASSERT_EQUAL_UINT32(1, dec.checksumErrors());
    TEST_ASSERT_EQUAL_UINT16(0, data.pack_voltage_dv);
}

void test_resync_after_garbage(void) {
    const uint8_t payload[8] = {0x00, 0x82, 0x00, 0x00, 0x75, 0x30, 0x01, 0xF4};
    uint8_t frame[FRAME_LEN];
    makeFrame(0x01, CMD_PACK, payload, frame);

    FrameDecoder dec;
    BmsData data;

    // Line noise, including a false 0xA5 that fails the command-range check.
    const uint8_t noise[] = {0x00, 0xFF, 0xA5, 0x11, 0x22, 0x33};
    for (size_t i = 0; i < sizeof(noise); i++) {
        TEST_ASSERT_FALSE(dec.feed(noise[i]));
    }

    TEST_ASSERT_TRUE_MESSAGE(feedFrame(dec, frame, data),
                             "decoder must resync onto the next good frame");
    TEST_ASSERT_EQUAL_UINT16(130, data.pack_voltage_dv);
}

void test_frame_immediately_after_frame(void) {
    const uint8_t payload[8] = {0x00, 0x82, 0x00, 0x00, 0x75, 0x30, 0x01, 0xF4};
    uint8_t frame[FRAME_LEN];
    makeFrame(0x01, CMD_PACK, payload, frame);

    FrameDecoder dec;
    BmsData data;
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
}

// ---------------------------------------------------------------- decoding

void test_discharge_current_is_negative(void) {
    // 30000 - 125 => -12.5 A
    const uint8_t payload[8] = {0x01, 0x5E, 0x00, 0x00, 0x74, 0xB3, 0x03, 0x20};
    uint8_t frame[FRAME_LEN];
    makeFrame(0x01, CMD_PACK, payload, frame);

    FrameDecoder dec;
    BmsData data;
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    TEST_ASSERT_EQUAL_UINT16(350, data.pack_voltage_dv); // 35.0 V
    TEST_ASSERT_EQUAL_INT16(-125, data.pack_current_da); // -12.5 A
    TEST_ASSERT_EQUAL_UINT16(800, data.soc_dpct);        // 80.0 %
}

void test_min_max_cell(void) {
    // max 3345 mV @ cell 4, min 3298 mV @ cell 7
    const uint8_t payload[8] = {0x0D, 0x11, 0x04, 0x0C, 0xE2, 0x07, 0x00, 0x00};
    uint8_t frame[FRAME_LEN];
    makeFrame(0x01, CMD_MINMAX_CELL, payload, frame);

    FrameDecoder dec;
    BmsData data;
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    TEST_ASSERT_EQUAL_UINT16(3345, data.max_cell_mv);
    TEST_ASSERT_EQUAL_UINT8(4, data.max_cell_idx);
    TEST_ASSERT_EQUAL_UINT16(3298, data.min_cell_mv);
    TEST_ASSERT_EQUAL_UINT8(7, data.min_cell_idx);
}

void test_temperatures_including_negative(void) {
    // Both values carry a +40 offset: 65-40 = 25 C, 35-40 = -5 C.
    const uint8_t payload[8] = {65, 0x01, 35, 0x02, 0x00, 0x00, 0x00, 0x00};
    uint8_t frame[FRAME_LEN];
    makeFrame(0x01, CMD_TEMP, payload, frame);

    FrameDecoder dec;
    BmsData data;
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    TEST_ASSERT_EQUAL_INT16(25, data.max_temp_c);
    TEST_ASSERT_EQUAL_UINT8(1, data.max_temp_idx);
    TEST_ASSERT_EQUAL_INT16(-5, data.min_temp_c);
    TEST_ASSERT_EQUAL_UINT8(2, data.min_temp_idx);
}

void test_mos_status_and_remaining_capacity(void) {
    // discharging, charge MOS off, discharge MOS on, 100000 mAh remaining
    const uint8_t payload[8] = {0x02, 0x00, 0x01, 0x7B, 0x00, 0x01, 0x86, 0xA0};
    uint8_t frame[FRAME_LEN];
    makeFrame(0x01, CMD_MOS, payload, frame);

    FrameDecoder dec;
    BmsData data;
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    TEST_ASSERT_EQUAL_UINT8(STATE_DISCHARGE, data.charge_state);
    TEST_ASSERT_FALSE(data.charge_mos);
    TEST_ASSERT_TRUE(data.discharge_mos);
    TEST_ASSERT_EQUAL_UINT8(0x7B, data.bms_life);
    TEST_ASSERT_EQUAL_UINT32(100000, data.remaining_mah);
}

void test_status_counts(void) {
    // 8 cells, 2 temp sensors, no charger, load present, 137 cycles
    const uint8_t payload[8] = {0x08, 0x02, 0x00, 0x01, 0x00, 0x00, 0x89, 0x00};
    uint8_t frame[FRAME_LEN];
    makeFrame(0x01, CMD_STATUS, payload, frame);

    FrameDecoder dec;
    BmsData data;
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    TEST_ASSERT_EQUAL_UINT8(8, data.cell_count);
    TEST_ASSERT_EQUAL_UINT8(2, data.temp_sensor_count);
    TEST_ASSERT_FALSE(data.charger_present);
    TEST_ASSERT_TRUE(data.load_present);
    TEST_ASSERT_EQUAL_UINT16(137, data.cycles);
}

// ------------------------------------------------- 0x95 multi-frame handling

void test_cell_voltages_multiframe(void) {
    FrameDecoder dec;
    BmsData data;
    uint8_t frame[FRAME_LEN];

    // Pack size first, so padding in the final frame gets rejected outright.
    const uint8_t status[8] = {0x04, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    makeFrame(0x01, CMD_STATUS, status, frame);
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));

    // Frame 1 -> cells 1..3
    const uint8_t f1[8] = {0x01, 0x0C, 0xE4, 0x0C, 0xE5, 0x0C, 0xE6, 0x00};
    makeFrame(0x01, CMD_CELL_VOLTS, f1, frame);
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));

    // Frame 2 -> cell 4, then two padding slots
    const uint8_t f2[8] = {0x02, 0x0C, 0xE7, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    makeFrame(0x01, CMD_CELL_VOLTS, f2, frame);
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));

    TEST_ASSERT_EQUAL_UINT8(4, data.cells_reported);
    TEST_ASSERT_EQUAL_UINT16(3300, data.cell_mv[0]);
    TEST_ASSERT_EQUAL_UINT16(3301, data.cell_mv[1]);
    TEST_ASSERT_EQUAL_UINT16(3302, data.cell_mv[2]);
    TEST_ASSERT_EQUAL_UINT16(3303, data.cell_mv[3]);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, data.cell_mv[4], "padding must not be stored");
}

// The BMS may answer 0x95 before we have ever seen 0x94, so the padding cells
// cannot be filtered on the way in. 0x94 must retroactively trim them.
void test_cell_voltages_before_status_are_trimmed(void) {
    FrameDecoder dec;
    BmsData data;
    uint8_t frame[FRAME_LEN];

    const uint8_t f1[8] = {0x01, 0x0C, 0xE4, 0x0C, 0xE5, 0x0C, 0xE6, 0x00};
    makeFrame(0x01, CMD_CELL_VOLTS, f1, frame);
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    const uint8_t f2[8] = {0x02, 0x0C, 0xE7, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    makeFrame(0x01, CMD_CELL_VOLTS, f2, frame);
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));

    TEST_ASSERT_EQUAL_UINT8(6, data.cells_reported); // padding accepted for now

    const uint8_t status[8] = {0x04, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    makeFrame(0x01, CMD_STATUS, status, frame);
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));

    TEST_ASSERT_EQUAL_UINT8(4, data.cells_reported);
    TEST_ASSERT_EQUAL_UINT16(3303, data.cell_mv[3]);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, data.cell_mv[4], "padding must be trimmed");
    TEST_ASSERT_EQUAL_UINT16(0, data.cell_mv[5]);
}

void test_cell_voltage_frame_cannot_overflow_buffer(void) {
    FrameDecoder dec;
    BmsData data;
    uint8_t frame[FRAME_LEN];

    // Frame 99 would address cells 295..297. Must be dropped, not written.
    const uint8_t bogus[8] = {99, 0x0C, 0xE4, 0x0C, 0xE5, 0x0C, 0xE6, 0x00};
    makeFrame(0x01, CMD_CELL_VOLTS, bogus, frame);
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    TEST_ASSERT_EQUAL_UINT8(0, data.cells_reported);
}

// ------------------------------------------------------ protection limits

void test_protection_limits_decoded(void) {
    // cell low L1 2900 mV, L2 2700 mV; pack low L1 46.4 V, L2 43.2 V
    const uint8_t cell[8] = {0x0E, 0x10, 0x0E, 0x74, 0x0B, 0x54, 0x0A, 0x8C};
    const uint8_t pack[8] = {0x02, 0x58, 0x02, 0x58, 0x01, 0xD0, 0x01, 0xB0};
    uint8_t frame[FRAME_LEN];
    FrameDecoder dec;
    BmsData data;

    makeFrame(0x01, CMD_CELL_THRESH, cell, frame);
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    TEST_ASSERT_EQUAL_UINT16(2900, data.cell_low_l1_mv);
    TEST_ASSERT_EQUAL_UINT16(2700, data.cell_low_l2_mv);

    makeFrame(0x01, CMD_PACK_THRESH, pack, frame);
    TEST_ASSERT_TRUE(feedFrame(dec, frame, data));
    TEST_ASSERT_EQUAL_UINT16(464, data.pack_low_l1_dv);
    TEST_ASSERT_EQUAL_UINT16(432, data.pack_low_l2_dv);
}

// ---------------------------------------------------------------- runner

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_checksum_matches_real_capture);
    RUN_TEST(test_build_request);
    RUN_TEST(test_response_address_byte_is_not_matched);
    RUN_TEST(test_address_byte_01_also_accepted);
    RUN_TEST(test_bad_checksum_rejected);
    RUN_TEST(test_resync_after_garbage);
    RUN_TEST(test_frame_immediately_after_frame);

    RUN_TEST(test_discharge_current_is_negative);
    RUN_TEST(test_min_max_cell);
    RUN_TEST(test_temperatures_including_negative);
    RUN_TEST(test_mos_status_and_remaining_capacity);
    RUN_TEST(test_status_counts);

    RUN_TEST(test_cell_voltages_multiframe);
    RUN_TEST(test_cell_voltages_before_status_are_trimmed);
    RUN_TEST(test_cell_voltage_frame_cannot_overflow_buffer);

    RUN_TEST(test_protection_limits_decoded);

    return UNITY_END();
}
