#include "daly_protocol.h"

namespace daly {
namespace {

inline uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // namespace

uint8_t checksum(const uint8_t* buf, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = static_cast<uint8_t>(sum + buf[i]);
    }
    return sum;
}

void buildRequest(uint8_t cmd, uint8_t* out) {
    out[0] = START_BYTE;
    out[1] = HOST_ADDR;
    out[2] = cmd;
    out[3] = PAYLOAD_LEN;
    for (uint8_t i = 4; i < FRAME_LEN - 1; i++) {
        out[i] = 0x00;
    }
    out[FRAME_LEN - 1] = checksum(out, FRAME_LEN - 1);
}

bool FrameDecoder::prefixPlausible() const {
    if (len_ >= 1 && buf_[0] != START_BYTE) return false;
    // buf_[1] is the address byte and is intentionally not checked; see the
    // class comment in daly_protocol.h.
    if (len_ >= 3 && !isKnownCommand(buf_[2])) return false;
    if (len_ >= 4 && buf_[3] != PAYLOAD_LEN) return false;
    return true;
}

void FrameDecoder::shiftOne() {
    if (len_ == 0) return;
    len_--;
    for (uint8_t i = 0; i < len_; i++) {
        buf_[i] = buf_[i + 1];
    }
}

bool FrameDecoder::feed(uint8_t b) {
    buf_[len_++] = b;

    // Drop leading bytes until what we hold could still become a valid frame.
    while (len_ > 0 && !prefixPlausible()) {
        shiftOne();
    }
    if (len_ < FRAME_LEN) return false;

    if (checksum(buf_, FRAME_LEN - 1) == buf_[FRAME_LEN - 1]) {
        len_ = 0; // buf_ stays intact for frame() until the next feed()
        return true;
    }

    // Bad checksum. The 0xA5 we locked onto was probably payload from a
    // previous frame, so drop it and rescan what we already have rather than
    // discarding the whole buffer.
    checksum_errors_++;
    shiftOne();
    while (len_ > 0 && !prefixPlausible()) {
        shiftOne();
    }
    return false;
}

uint8_t applyFrame(const uint8_t* frame, BmsData& out) {
    const uint8_t cmd = frame[2];
    const uint8_t* d = frame + 4; // 8 payload bytes

    switch (cmd) {
    case CMD_PACK:
        out.pack_voltage_dv = be16(d);
        // d[2..3] is the "gather voltage", which duplicates the pack voltage on
        // most models. Ignored.
        out.pack_current_da = static_cast<int16_t>(static_cast<int32_t>(be16(d + 4)) - 30000);
        out.soc_dpct = be16(d + 6);
        return cmd;

    case CMD_MINMAX_CELL:
        out.max_cell_mv = be16(d);
        out.max_cell_idx = d[2];
        out.min_cell_mv = be16(d + 3);
        out.min_cell_idx = d[5];
        return cmd;

    case CMD_TEMP:
        out.max_temp_c = static_cast<int16_t>(static_cast<int16_t>(d[0]) - 40);
        out.max_temp_idx = d[1];
        out.min_temp_c = static_cast<int16_t>(static_cast<int16_t>(d[2]) - 40);
        out.min_temp_idx = d[3];
        return cmd;

    case CMD_MOS:
        out.charge_state = d[0];
        out.charge_mos = d[1] != 0;
        out.discharge_mos = d[2] != 0;
        out.bms_life = d[3];
        out.remaining_mah = be32(d + 4);
        return cmd;

    case CMD_STATUS:
        out.cell_count = d[0] > MAX_CELLS ? MAX_CELLS : d[0];
        out.temp_sensor_count = d[1];
        out.charger_present = d[2] != 0;
        out.load_present = d[3] != 0;
        // d[4] is DI/DO bit state, unused.
        out.cycles = be16(d + 5);
        // If 0x95 ran before we knew the pack size it will have accepted the
        // padding cells in the final frame. Now that we know, drop them.
        if (out.cell_count != 0 && out.cells_reported > out.cell_count) {
            for (uint8_t i = out.cell_count; i < out.cells_reported; i++) {
                out.cell_mv[i] = 0;
            }
            out.cells_reported = out.cell_count;
        }
        return cmd;

    case CMD_CELL_VOLTS: {
        // Multi-frame: d[0] is a 1-based frame number carrying three cells.
        const uint8_t frame_no = d[0];
        if (frame_no == 0) return cmd; // some units emit a padding frame
        // uint16_t, not uint8_t: a corrupt frame number must be caught by the
        // bounds check below rather than wrapping into a valid cell index.
        const uint16_t first_cell = static_cast<uint16_t>((frame_no - 1) * 3); // 0-based
        for (uint16_t i = 0; i < 3; i++) {
            const uint16_t cell = static_cast<uint16_t>(first_cell + i);
            if (cell >= MAX_CELLS) break;
            // Trailing cells in the last frame are padding once we know the
            // pack size; without 0x94 yet, accept them and let 0x94 trim later.
            if (out.cell_count != 0 && cell >= out.cell_count) break;
            out.cell_mv[cell] = be16(d + 1 + i * 2);
            if (cell + 1 > out.cells_reported) {
                out.cells_reported = static_cast<uint8_t>(cell + 1);
            }
        }
        return cmd;
    }

    // Payload is high L1, high L2, low L1, low L2, each big-endian.
    case CMD_CELL_THRESH:
        out.cell_low_l1_mv = be16(d + 4);
        out.cell_low_l2_mv = be16(d + 6);
        return cmd;

    case CMD_PACK_THRESH:
        out.pack_low_l1_dv = be16(d + 4);
        out.pack_low_l2_dv = be16(d + 6);
        return cmd;

    default:
        return 0;
    }
}

} // namespace daly
