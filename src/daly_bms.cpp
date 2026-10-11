#include "daly_bms.h"

#include "applog.h"

namespace {

const uint8_t kCommands[DalyBms::kCommandCount] = {
    daly::CMD_PACK, daly::CMD_MINMAX_CELL, daly::CMD_TEMP,
    daly::CMD_MOS,  daly::CMD_STATUS,      daly::CMD_CELL_VOLTS,
    daly::CMD_CELL_THRESH, daly::CMD_PACK_THRESH,
};

} // namespace

void DalyBms::begin(Stream* serial, uint32_t poll_interval_ms, uint32_t stale_after_ms) {
    serial_ = serial;
    poll_interval_ms_ = poll_interval_ms;
    stale_after_ms_ = stale_after_ms;
    state_ = State::Idle;
    // Backdate so the first sweep fires immediately rather than one interval in.
    sweep_started_at_ = millis() - poll_interval_ms;
}

void DalyBms::setPolling(uint32_t poll_interval_ms, uint32_t stale_after_ms) {
    poll_interval_ms_ = poll_interval_ms;
    stale_after_ms_ = stale_after_ms;
}

uint32_t DalyBms::msUntilSweep() const {
    if (state_ != State::Idle) return 0;
    const uint32_t since = millis() - sweep_started_at_;
    return since >= poll_interval_ms_ ? 0 : poll_interval_ms_ - since;
}

void DalyBms::sendCurrentCommand() {
    uint8_t req[daly::FRAME_LEN];
    daly::buildRequest(kCommands[cmd_idx_], req);

    // Drop anything left over from the previous exchange so a late reply cannot
    // be mistaken for the answer to this request.
    while (serial_->available()) serial_->read();
    decoder_.reset();

    serial_->write(req, daly::FRAME_LEN);

    sent_at_ = millis();
    got_response_ = false;
    state_ = State::Waiting;
}

void DalyBms::advance() {
    cmd_idx_++;
    if (cmd_idx_ >= (needsLimits() ? kCommandCount : kLiveCommandCount)) {
        if (needsLimits()) limit_sweeps_++;
        cmd_idx_ = 0;
        sweeps_++;
        state_ = State::Idle;
        return;
    }
    sendCurrentCommand();
}

void DalyBms::loop() {
    if (serial_ == nullptr) return;
    const uint32_t now = millis();

    switch (state_) {
    case State::Idle:
        if (now - sweep_started_at_ >= poll_interval_ms_) {
            sweep_started_at_ = now;
            cmd_idx_ = 0;
            sendCurrentCommand();
        }
        return;

    case State::Waiting:
        while (serial_->available()) {
            if (!decoder_.feed(static_cast<uint8_t>(serial_->read()))) continue;

            const uint8_t* f = decoder_.frame();
            frames_ok_++;
            ever_good_ = true;
            last_good_ms_ = now;

            // Apply whatever arrived, even if it answers an earlier command:
            // it passed the checksum, so it is real data worth keeping.
            daly::applyFrame(f, data_);

            if (f[2] == kCommands[cmd_idx_]) {
                got_response_ = true;
                // 0x95 spans several frames, so keep reading until the timeout
                // instead of moving on after the first one.
                if (kCommands[cmd_idx_] != daly::CMD_CELL_VOLTS) {
                    advance();
                    return;
                }
            }
        }

        if (now - sent_at_ >= kResponseTimeoutMs) {
            if (!got_response_) {
                timeouts_++;
                APPLOG("timeout on cmd 0x%02X", kCommands[cmd_idx_]);
            }
            advance();
        }
        return;
    }
}

bool DalyBms::valid() const {
    if (!ever_good_) return false;
    return (millis() - last_good_ms_) < stale_after_ms_;
}

uint16_t DalyBms::secondsSinceGoodFrame() const {
    if (!ever_good_) return 65535;
    const uint32_t s = (millis() - last_good_ms_) / 1000;
    return s > 65534 ? 65534 : static_cast<uint16_t>(s);
}
