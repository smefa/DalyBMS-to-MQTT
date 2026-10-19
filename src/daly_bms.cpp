#include "daly_bms.h"

#include "applog.h"

namespace {

const uint8_t kCommands[DalyBms::kCommandCount] = {
    daly::CMD_PACK, daly::CMD_MINMAX_CELL, daly::CMD_TEMP,
    daly::CMD_MOS,  daly::CMD_STATUS,      daly::CMD_CELL_VOLTS,
    daly::CMD_CELL_THRESH, daly::CMD_PACK_THRESH, daly::CMD_SLEEP_CFG,
};

} // namespace

uint8_t DalyBms::commandAt(uint8_t i) { return i < kCommandCount ? kCommands[i] : 0; }

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
    cell_frames_seen_ = 0;
    state_ = State::Waiting;
}

bool DalyBms::queueWrite(uint8_t cmd, const daly::Thresholds& t) {
    if (writePending()) return false;
    const bool ok = cmd == daly::CMD_SET_CELL_THRESH ? daly::validCellThresholds(t)
                                                     : daly::validPackThresholds(t);
    if (!ok) return false;
    write_cmd_ = cmd;
    write_values_ = t;
    write_state_ = WriteState::Queued;
    write_serial_++;
    return true;
}

bool DalyBms::queueSleepTimeout(uint16_t seconds) {
    if (writePending() || !data_.have_sleep || seconds < daly::SLEEP_TIMEOUT_MIN_S) return false;
    write_cmd_ = daly::CMD_SET_SLEEP_CFG;
    write_timeout_s_ = seconds;
    write_state_ = WriteState::Queued;
    write_serial_++;
    return true;
}

bool DalyBms::queueMosfet(bool charge, bool on) {
    if (writePending()) return false;
    write_cmd_ = charge ? daly::CMD_SET_CHARGE_MOS : daly::CMD_SET_DISCHARGE_MOS;
    write_mos_on_ = on;
    write_state_ = WriteState::Queued;
    write_serial_++;
    return true;
}

bool DalyBms::queueCellLimits(const daly::Thresholds& t) {
    return queueWrite(daly::CMD_SET_CELL_THRESH, t);
}

bool DalyBms::queuePackLimits(const daly::Thresholds& t) {
    return queueWrite(daly::CMD_SET_PACK_THRESH, t);
}

// True when the limits we last read already equal the queued values.
bool DalyBms::writeIsNoop() const {
    if (writeIsMos()) return false;
    if (write_cmd_ == daly::CMD_SET_SLEEP_CFG) {
        return data_.have_sleep && data_.sleep_timeout_s == write_timeout_s_;
    }
    const daly::Thresholds& v = write_values_;
    if (write_cmd_ == daly::CMD_SET_CELL_THRESH) {
        return data_.cell_low_l1_mv != 0 && data_.cell_high_l1_mv == v.high_l1 &&
               data_.cell_high_l2_mv == v.high_l2 && data_.cell_low_l1_mv == v.low_l1 &&
               data_.cell_low_l2_mv == v.low_l2;
    }
    return data_.pack_low_l1_dv != 0 && data_.pack_high_l1_dv == v.high_l1 &&
           data_.pack_high_l2_dv == v.high_l2 && data_.pack_low_l1_dv == v.low_l1 &&
           data_.pack_low_l2_dv == v.low_l2;
}

void DalyBms::sendWrite() {
    uint8_t req[daly::FRAME_LEN];
    const char* what = "sleep timeout";
    if (writeIsMos()) {
        daly::buildMosWrite(write_cmd_, write_mos_on_, req);
        what = write_cmd_ == daly::CMD_SET_CHARGE_MOS ? (write_mos_on_ ? "charge MOSFET on" : "charge MOSFET off")
                                                      : (write_mos_on_ ? "discharge MOSFET on" : "discharge MOSFET off");
    } else if (write_cmd_ == daly::CMD_SET_SLEEP_CFG) {
        daly::buildSleepTimeoutWrite(data_.sleep_raw, write_timeout_s_, req);
    } else {
        daly::buildThresholdWrite(write_cmd_, write_values_, req);
        what = write_cmd_ == daly::CMD_SET_CELL_THRESH ? "cell limits" : "pack limits";
    }
    while (serial_->available()) serial_->read();
    decoder_.reset();
    serial_->write(req, daly::FRAME_LEN);
    APPLOG("bms: wrote %s", what);
    sent_at_ = millis();
    got_response_ = false;
    writing_ = true;
    state_ = State::Waiting;
}

// The write reply says nothing about success, so the verdict is whatever the next
// read of the limits shows.
void DalyBms::checkReadback(uint8_t applied) {
    if (write_state_ != WriteState::Verifying) return;
    if (writeIsMos()) {
        // The BMS may take a moment to move the switch: a reading that still shows the
        // old state is not a verdict, the next sweep tries again until the deadline.
        if (applied != daly::CMD_MOS) return;
        const bool now_on = write_cmd_ == daly::CMD_SET_CHARGE_MOS ? data_.charge_mos : data_.discharge_mos;
        if (now_on != write_mos_on_) return;
        write_state_ = WriteState::Done;
        APPLOG("bms: MOSFET read back ok");
        return;
    }
    if (applied != write_cmd_ + 0x40) return;
    if (applied == daly::CMD_SLEEP_CFG) {
        const bool ok = data_.sleep_timeout_s == write_timeout_s_;
        write_state_ = ok ? WriteState::Done : WriteState::Mismatch;
        APPLOG("bms: sleep timeout read back %s", ok ? "ok" : "DIFFERENT");
        return;
    }
    const daly::Thresholds& v = write_values_;
    const bool cell = applied == daly::CMD_CELL_THRESH;
    const bool match = cell ? (data_.cell_high_l1_mv == v.high_l1 && data_.cell_high_l2_mv == v.high_l2 &&
                               data_.cell_low_l1_mv == v.low_l1 && data_.cell_low_l2_mv == v.low_l2)
                            : (data_.pack_high_l1_dv == v.high_l1 && data_.pack_high_l2_dv == v.high_l2 &&
                               data_.pack_low_l1_dv == v.low_l1 && data_.pack_low_l2_dv == v.low_l2);
    write_state_ = match ? WriteState::Done : WriteState::Mismatch;
    APPLOG("bms: %s limits read back %s", cell ? "cell" : "pack", match ? "ok" : "DIFFERENT");
}

bool DalyBms::cellFramesComplete() const {
    if (data_.cell_count == 0) return false;
    const uint16_t want = daly::cellFramesMask(data_.cell_count);
    return (cell_frames_seen_ & want) == want;
}

void DalyBms::advance() {
    cmd_idx_++;
    if (cmd_idx_ >= (needsLimits() ? kCommandCount : kLiveCommandCount)) {
        if (needsLimits()) limit_sweeps_++;
        if (write_state_ != WriteState::Verifying) force_limits_ = false;
        cmd_idx_ = 0;
        sweeps_++;
        // Giving up is not for good: a BMS that was slow to answer after a reboot would
        // otherwise leave the limits (and the pack profile) unknown until the next one.
        if (limit_sweeps_ >= kMaxLimitSweeps && data_.pack_low_l1_dv == 0 &&
            sweeps_ % kLimitRetryEvery == 0) {
            limit_sweeps_ = 0;
        }
        state_ = State::Idle;
        return;
    }
    sendCurrentCommand();
}

void DalyBms::loop() {
    if (serial_ == nullptr) return;
    const uint32_t now = millis();

    // An old min/max cell reading is worse than none: the guard falls back to the
    // pack total when min_cell_mv is 0, but would trust a stale healthy cell value
    // forever if 0x91 stopped answering while the other replies kept coming.
    if (have_minmax_ && now - minmax_ms_ >= stale_after_ms_) {
        have_minmax_ = false;
        data_.min_cell_mv = 0;
        data_.max_cell_mv = 0;
        data_.min_cell_idx = 0;
        data_.max_cell_idx = 0;
    }

    if (write_state_ == WriteState::Verifying && static_cast<int32_t>(now - verify_deadline_) >= 0) {
        write_state_ = WriteState::Mismatch;
        APPLOG("bms: write not read back in time");
    }

    switch (state_) {
    case State::Idle: {
        // Writes go out between sweeps, and only after one has finished so the
        // no-change check has the current limits to compare with.
        if (write_state_ == WriteState::Queued && sweeps_ > 0) {
            if (writeIsNoop()) {
                write_state_ = WriteState::Unchanged;
            } else {
                sendWrite();
                return;
            }
        }
        // A MOSFET read-back keeps sweeping every couple of seconds until the switch shows
        // (or the deadline), however slow the normal polling is.
        const uint32_t gap = (write_state_ == WriteState::Verifying && writeIsMos())
                                 ? min<uint32_t>(poll_interval_ms_, kMosVerifyGapMs)
                                 : poll_interval_ms_;
        if (now - sweep_started_at_ >= gap) {
            sweep_started_at_ = now;
            cmd_idx_ = 0;
            sendCurrentCommand();
        }
        return;
    }

    case State::Waiting:
        if (writing_) {
            // The reply is a normal frame with the write's command byte. Its
            // payload means nothing, so either it or the timeout ends the wait;
            // the read-back decides.
            while (serial_->available()) {
                if (!decoder_.feed(static_cast<uint8_t>(serial_->read()))) continue;
                frames_ok_++;
                ever_good_ = true;
                last_good_ms_ = now;
                if (decoder_.frame()[2] == write_cmd_) got_response_ = true;
            }
            // Errors in a write's reply belong to no polled command.
            crc_seen_ = decoder_.checksumErrors();
            if (got_response_ || now - sent_at_ >= kResponseTimeoutMs) {
                writing_ = false;
                write_state_ = WriteState::Verifying;
                verify_deadline_ = now + 30000;
                // The MOSFET state is part of every sweep; the limits need not be re-read.
                if (!writeIsMos()) {
                    force_limits_ = true;
                    limit_sweeps_ = 0;
                }
                state_ = State::Idle;
                sweep_started_at_ = now - poll_interval_ms_; // sweep right away
            }
            return;
        }
        while (serial_->available()) {
            const bool framed = decoder_.feed(static_cast<uint8_t>(serial_->read()));
            const uint32_t crc = decoder_.checksumErrors();
            if (crc != crc_seen_) {
                cmd_crc_[cmd_idx_] += crc - crc_seen_;
                crc_seen_ = crc;
            }
            if (!framed) continue;

            const uint8_t* f = decoder_.frame();
            frames_ok_++;
            ever_good_ = true;
            last_good_ms_ = now;

            // Apply whatever arrived, even if it answers an earlier command:
            // it passed the checksum, so it is real data worth keeping.
            const uint8_t applied = daly::applyFrame(f, data_);
            checkReadback(applied);
            for (uint8_t i = 0; i < kLiveCommandCount; i++) {
                // 0x95 counts once its last frame is in, not on the first one.
                if (applied == kCommands[i] && kCommands[i] != daly::CMD_CELL_VOLTS) {
                    live_seen_ |= static_cast<uint8_t>(1U << i);
                }
            }
            if (applied == daly::CMD_PACK) {
                have_pack_ = true;
                pack_missed_ = false;
                pack_ms_ = now;
            } else if (f[2] == daly::CMD_PACK) {
                // Rejected pack frame (all zero): a half-awake BMS, same as a missed
                // request. It wakes the BMS at once, but the last good values stay
                // until they go stale: dropping them here told Home Assistant
                // "unavailable" for the second or two the wake pulse takes.
                pack_missed_ = true;
            } else if (applied == daly::CMD_MINMAX_CELL) {
                have_minmax_ = true;
                minmax_ms_ = now;
            }

            if (f[2] == kCommands[cmd_idx_]) {
                got_response_ = true;
                // 0x95 spans several frames, three cells each. Once the pack size
                // is known, move on when the last one is in; until then, and if
                // one goes missing, keep reading until the timeout.
                if (kCommands[cmd_idx_] != daly::CMD_CELL_VOLTS) {
                    advance();
                    return;
                }
                cell_frames_seen_ |= daly::cellFrameBit(f[4]);
                if (cellFramesComplete()) {
                    live_seen_ |= static_cast<uint8_t>(1U << cmd_idx_);
                    advance();
                    return;
                }
            }
        }

        if (now - sent_at_ >= kResponseTimeoutMs) {
            // A 0x95 with frames missing is a miss too: the cell list is short.
            const bool cells_short =
                kCommands[cmd_idx_] == daly::CMD_CELL_VOLTS && data_.cell_count != 0 && !cellFramesComplete();
            if (!got_response_ || cells_short) {
                timeouts_++;
                cmd_timeouts_[cmd_idx_]++;
                if (kCommands[cmd_idx_] == daly::CMD_PACK) pack_missed_ = true;
                // Log the first miss and then one a minute: a silent BMS otherwise
                // fills the 16-line ring in seconds and pushes out the boot reason.
                if (timeouts_ == 1 || now - last_timeout_log_ms_ >= 60000) {
                    last_timeout_log_ms_ = now;
                    APPLOG("timeout on cmd 0x%02X (%lu total)", kCommands[cmd_idx_],
                           static_cast<unsigned long>(timeouts_));
                }
            }
            advance();
        }
        return;
    }
}

bool DalyBms::valid() const {
    return have_pack_ && (millis() - pack_ms_) < stale_after_ms_;
}

bool DalyBms::complete() const { return have_pack_ && sweeps_ > 0; }

uint16_t DalyBms::secondsSincePack() const {
    if (!have_pack_) return 65535;
    const uint32_t s = (millis() - pack_ms_) / 1000;
    return s > 65534 ? 65534 : static_cast<uint16_t>(s);
}

uint16_t DalyBms::secondsSinceGoodFrame() const {
    if (!ever_good_) return 65535;
    const uint32_t s = (millis() - last_good_ms_) / 1000;
    return s > 65534 ? 65534 : static_cast<uint16_t>(s);
}
