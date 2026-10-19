// DALY BMS UART/RS485 protocol, V1.2.
//
// Deliberately free of any Arduino dependency: this compiles and unit-tests on
// the host. Anything needing a Stream, a clock or WiFi belongs in src/, not here.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace daly {

constexpr uint8_t FRAME_LEN = 13;   // fixed, both directions
constexpr uint8_t START_BYTE = 0xA5;
constexpr uint8_t HOST_ADDR = 0x40; // "UPPER-ADD", what we put in requests
constexpr uint8_t PAYLOAD_LEN = 0x08;

constexpr uint8_t CMD_MIN = 0x90;
constexpr uint8_t CMD_MAX = 0x98;

enum Command : uint8_t {
    CMD_PACK = 0x90,       // pack voltage / current / SOC
    CMD_MINMAX_CELL = 0x91,// highest + lowest cell voltage
    CMD_TEMP = 0x92,       // highest + lowest temperature
    CMD_MOS = 0x93,        // charge/discharge state, MOSFETs, remaining capacity
    CMD_STATUS = 0x94,     // cell count, sensor count, cycles
    CMD_CELL_VOLTS = 0x95, // per-cell voltages, multi-frame
    // Protection settings, read once: they only change when someone reprograms the BMS.
    CMD_CELL_THRESH = 0x59,// cell over/under-voltage, two levels each (mV)
    CMD_PACK_THRESH = 0x5A,// pack over/under-voltage, two levels each (0.1 V)
    // Writes: the read command minus 0x40. The reply is a normal frame with the
    // same command byte whose payload says nothing about success; read back.
    CMD_SET_CELL_THRESH = 0x19,
    CMD_SET_PACK_THRESH = 0x1A,
    // Sleep enable, sleep timeout (s) and current wave share one frame. Layout from a
    // forum capture, not the spec: see docs/daly-write-commands.md.
    CMD_SLEEP_CFG = 0x53,
    CMD_SET_SLEEP_CFG = 0x13,
};

inline bool isKnownCommand(uint8_t cmd) {
    return (cmd >= CMD_MIN && cmd <= CMD_MAX) || cmd == CMD_CELL_THRESH ||
           cmd == CMD_PACK_THRESH || cmd == CMD_SET_CELL_THRESH || cmd == CMD_SET_PACK_THRESH ||
           cmd == CMD_SLEEP_CFG || cmd == CMD_SET_SLEEP_CFG;
}

constexpr uint8_t MAX_CELLS = 32;

// 0x95 sends three cells per frame, numbered from 1. These let the caller know
// when a reply is complete instead of waiting out a timeout.
constexpr uint8_t cellFramesFor(uint8_t cells) { return static_cast<uint8_t>((cells + 2) / 3); }
// Bit (n - 1) stands for frame n. 0 for a number no pack of MAX_CELLS can send.
constexpr uint16_t cellFrameBit(uint8_t frame_no) {
    return (frame_no >= 1 && frame_no <= cellFramesFor(MAX_CELLS))
               ? static_cast<uint16_t>(1U << (frame_no - 1))
               : static_cast<uint16_t>(0);
}
// Every frame a pack of `cells` sends; 0 while the count is unknown.
constexpr uint16_t cellFramesMask(uint8_t cells) {
    return static_cast<uint16_t>((1U << cellFramesFor(cells)) - 1U);
}

enum ChargeState : uint8_t {
    STATE_IDLE = 0,
    STATE_CHARGE = 1,
    STATE_DISCHARGE = 2,
};

// Everything the six polled commands yield. Integer-scaled throughout: avoiding
// floats keeps the parser exactly reproducible on host and target.
struct BmsData {
    // 0x90
    uint16_t pack_voltage_dv = 0; // 0.1 V
    int16_t pack_current_da = 0;  // 0.1 A, negative = discharging
    uint16_t soc_dpct = 0;        // 0.1 %

    // 0x91
    uint16_t max_cell_mv = 0;
    uint8_t max_cell_idx = 0; // 1-based, as reported
    uint16_t min_cell_mv = 0;
    uint8_t min_cell_idx = 0;

    // 0x92
    int16_t max_temp_c = 0;
    uint8_t max_temp_idx = 0;
    int16_t min_temp_c = 0;
    uint8_t min_temp_idx = 0;

    // 0x93
    uint8_t charge_state = STATE_IDLE;
    bool charge_mos = false;
    bool discharge_mos = false;
    uint8_t bms_life = 0; // free-running counter, proves the BMS is alive
    uint32_t remaining_mah = 0;

    // 0x94
    uint8_t cell_count = 0;
    uint8_t temp_sensor_count = 0;
    bool charger_present = false;
    bool load_present = false;
    uint16_t cycles = 0;

    // 0x95
    uint16_t cell_mv[MAX_CELLS] = {};
    uint8_t cells_reported = 0; // highest cell index populated so far

    // 0x59 / 0x5A: the BMS's own over- and under-voltage limits. 0 until the BMS
    // has answered. Level 1 is the early warning, level 2 the cutoff; they are
    // kept as reported because units differ on which is which.
    uint16_t cell_high_l1_mv = 0;
    uint16_t cell_high_l2_mv = 0;
    uint16_t pack_high_l1_dv = 0;
    uint16_t pack_high_l2_dv = 0;
    uint16_t cell_low_l1_mv = 0;
    uint16_t cell_low_l2_mv = 0;
    uint16_t pack_low_l1_dv = 0;
    uint16_t pack_low_l2_dv = 0;

    // 0x53: the raw 8 bytes are kept so a write can copy back everything it does
    // not change. 0 timeout with have_sleep false means "not read yet".
    bool have_sleep = false;
    uint8_t sleep_raw[8] = {};
    uint16_t sleep_timeout_s = 0;
};

// Sum of all preceding bytes, truncated to one byte.
uint8_t checksum(const uint8_t* buf, size_t len);

// Build the 13-byte request for `cmd`. `out` must have room for FRAME_LEN.
void buildRequest(uint8_t cmd, uint8_t* out);

// The four values of one threshold group, in the order the BMS stores them.
// Cells are in mV, packs in 0.1 V.
struct Thresholds {
    uint16_t high_l1;
    uint16_t high_l2;
    uint16_t low_l1;
    uint16_t low_l2;
};

// Sanity limits, so a typo cannot be written into the BMS. Deliberately wide:
// they catch unit mistakes (volts for millivolts), not bad chemistry choices.
constexpr uint16_t CELL_LIMIT_MIN_MV = 1500;
constexpr uint16_t CELL_LIMIT_MAX_MV = 4500;
constexpr uint16_t PACK_LIMIT_MIN_DV = 50;   // 5 V
constexpr uint16_t PACK_LIMIT_MAX_DV = 1000; // 100 V

// Every value in range, and every high level above every low level.
bool validCellThresholds(const Thresholds& t);
bool validPackThresholds(const Thresholds& t);

// Build the 13-byte write for a threshold group (CMD_SET_CELL_THRESH or
// CMD_SET_PACK_THRESH). Does not validate; call the valid*() functions first.
void buildThresholdWrite(uint8_t cmd, const Thresholds& t, uint8_t* out);

constexpr uint16_t SLEEP_TIMEOUT_MIN_S = 10;

// Build the 0x13 write: `current` is the payload of the last 0x53 reply, copied back
// unchanged except for the timeout in bytes 5-6 of the payload (frame bytes 9-10).
void buildSleepTimeoutWrite(const uint8_t current[8], uint16_t timeout_s, uint8_t* out);

// Byte-at-a-time frame assembler with resynchronisation.
//
// Validates the start byte, the command range, the payload length and the
// checksum. It deliberately does NOT validate the address byte: the BMS answers
// with its own physical address (0x01), not the 0x40 we sent it. Rejecting
// frames on that mismatch is esphome/esphome#12445, which was closed
// "not planned" and never fixed upstream.
class FrameDecoder {
public:
    // Returns true when `frame()` holds a complete, checksum-valid frame.
    bool feed(uint8_t b);

    const uint8_t* frame() const { return buf_; }
    void reset() { len_ = 0; }

    uint32_t checksumErrors() const { return checksum_errors_; }

private:
    bool prefixPlausible() const;
    void shiftOne();

    uint8_t buf_[FRAME_LEN] = {};
    uint8_t len_ = 0;
    uint32_t checksum_errors_ = 0;
};

// Decode a validated frame into `out`. Returns the command handled, or 0 if the
// command is one we do not decode or the payload is rejected as implausible
// (an all-zero 0x90 pack frame).
uint8_t applyFrame(const uint8_t* frame, BmsData& out);

} // namespace daly
