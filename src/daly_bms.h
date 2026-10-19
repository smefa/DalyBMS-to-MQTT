// Arduino-side driver: drives the pure parser in lib/daly over a Stream.
//
// Non-blocking. One command in flight at a time, round-robin over the six
// commands we poll, then idle until the next sweep is due.
#pragma once

#include <Arduino.h>

#include "daly_protocol.h"

class DalyBms {
public:
    // The six live readings, then the two protection-limit reads. The limits are
    // only asked for until the BMS has answered both (see needsLimits()).
    static constexpr uint8_t kCommandCount = 9;
    static constexpr uint8_t kLiveCommandCount = 6;

    // `serial` must already be begin()-ed at 9600 8N1.
    void begin(Stream* serial, uint32_t poll_interval_ms, uint32_t stale_after_ms);

    // Changes the cadence without disturbing a sweep that is under way.
    void setPolling(uint32_t poll_interval_ms, uint32_t stale_after_ms);

    void loop();

    // Starts the next sweep as soon as the current one (if any) is done, instead of
    // waiting out the poll interval.
    void requestSweep() { sweep_started_at_ = millis() - poll_interval_ms_; }

    // True while a request is out and its reply has not been collected. The CPU
    // must stay awake then: SoftwareSerial receives by interrupt and a light-sleeping
    // core would miss the reply.
    bool busy() const { return state_ == State::Waiting; }

    // Milliseconds until the next sweep starts, 0 if it is due (or under way).
    uint32_t msUntilSweep() const;

    const daly::BmsData& data() const { return data_; }

    // True once the pack total (0x90) has been answered, recently. Other frames
    // arriving do not count. The min/max cell voltages (0x91) that go stale on
    // their own are zeroed by loop(), which the guard reads as "unknown".
    bool valid() const;

    // True once a full first sweep has finished with 0x90 answered, so the cell
    // voltages, cell count and protection limits have had their chance to arrive.
    bool complete() const;
    // True once each of the six live commands has had a usable answer since boot,
    // so no reading is still at its zero default. A command that never answers keeps
    // this false; complete() is the one the guard uses.
    bool allLiveSeen() const { return live_seen_ == (1U << kLiveCommandCount) - 1U; }

    // Seconds since 0x90 last answered, saturating at 65535 (also "never").
    uint16_t secondsSincePack() const;

    // True when the latest 0x90 request went unanswered, until one is. A sleeping BMS
    // shows up here at the first missed sweep, long before valid() runs out.
    bool packMissed() const { return pack_missed_; }

    // Seconds since the last checksum-valid frame, saturating at 65535.
    // 65535 also means "never had one".
    uint16_t secondsSinceGoodFrame() const;

    // Changing the BMS's voltage limits. Only one write can be waiting; each is sent
    // between sweeps, then the limits are read back to check they took. False if the
    // values are out of range (see daly::valid*Thresholds) or a write is already
    // waiting. A write identical to what the BMS already holds is not sent.
    bool queueCellLimits(const daly::Thresholds& t);
    bool queuePackLimits(const daly::Thresholds& t);
    // The BMS sleep timeout in seconds. Needs the 0x53 reading first, because the
    // write carries the other settings of that frame back unchanged.
    bool queueSleepTimeout(uint16_t seconds);

    // Switches the charge or discharge MOSFET. Always sent, even if the last reading
    // says it is already in that state; the 0x93 reading afterwards is the verdict.
    bool queueMosfet(bool charge, bool on);

    enum class WriteState : uint8_t {
        None,      // nothing requested since boot
        Queued,    // waiting for a gap between sweeps
        Verifying, // sent, waiting for the read-back
        Done,      // read back and matches
        Unchanged, // the BMS already held these values
        Mismatch,  // read back differs: the BMS refused or ignored the write
    };
    WriteState writeState() const { return write_state_; }
    // Bumps every time a request is queued, so a caller can tell its own outcome from an older one.
    uint16_t writeSerial() const { return write_serial_; }
    // True from queueing until the outcome is known: the CPU must not sleep.
    bool writePending() const {
        return write_state_ == WriteState::Queued || write_state_ == WriteState::Verifying;
    }

    uint32_t framesOk() const { return frames_ok_; }
    uint32_t timeouts() const { return timeouts_; }
    uint32_t checksumErrors() const { return decoder_.checksumErrors(); }
    uint32_t sweeps() const { return sweeps_; }
    // Misses and bad checksums per command, indexed like the sweep order, so a
    // pattern (one command, or only after a wake) shows up in the stats.
    uint32_t timeoutsFor(uint8_t i) const { return i < kCommandCount ? cmd_timeouts_[i] : 0; }
    uint32_t checksumErrorsFor(uint8_t i) const { return i < kCommandCount ? cmd_crc_[i] : 0; }
    static uint8_t commandAt(uint8_t i);

private:
    enum class State : uint8_t { Idle, Waiting };

    // Older firmware may not implement 0x59/0x5A. Give up after a few sweeps so
    // it does not cost two timeouts and a log line on every one.
    static constexpr uint8_t kMaxLimitSweeps = 3;
    // While the pack limits are still unread, try again for another few sweeps this often.
    static constexpr uint32_t kLimitRetryEvery = 20;
    bool needsLimits() const {
        return force_limits_ || ((data_.cell_low_l1_mv == 0 || data_.pack_low_l1_dv == 0 ||
                                  !data_.have_sleep) &&
                                 limit_sweeps_ < kMaxLimitSweeps);
    }
    bool queueWrite(uint8_t cmd, const daly::Thresholds& t);
    void sendWrite();
    void checkReadback(uint8_t applied);
    bool writeIsNoop() const;
    bool writeIsMos() const {
        return write_cmd_ == daly::CMD_SET_CHARGE_MOS || write_cmd_ == daly::CMD_SET_DISCHARGE_MOS;
    }
    bool force_limits_ = false; // read the limits on the next sweep whatever else is true
    bool writing_ = false;      // the request in flight is a write
    uint8_t write_cmd_ = 0;
    bool write_mos_on_ = false;
    daly::Thresholds write_values_ = {};
    uint16_t write_timeout_s_ = 0;
    uint32_t verify_deadline_ = 0;
    WriteState write_state_ = WriteState::None;
    uint16_t write_serial_ = 0;
    uint8_t limit_sweeps_ = 0;
    void sendCurrentCommand();
    void advance();
    bool cellFramesComplete() const;

    // The BMS normally answers within ~30 ms. 0x95 streams several frames back
    // to back, and we deliberately wait out the whole window to collect them.
    static constexpr uint32_t kResponseTimeoutMs = 250;
    static constexpr uint32_t kMosVerifyGapMs = 2000;

    Stream* serial_ = nullptr;
    daly::FrameDecoder decoder_;
    daly::BmsData data_;

    uint32_t poll_interval_ms_ = 5000;
    uint32_t stale_after_ms_ = 30000;

    State state_ = State::Idle;
    uint8_t cmd_idx_ = 0;
    uint32_t sent_at_ = 0;
    uint32_t sweep_started_at_ = 0;
    bool got_response_ = false;

    bool ever_good_ = false;
    uint32_t last_good_ms_ = 0;
    uint32_t pack_ms_ = 0;
    uint32_t minmax_ms_ = 0;
    bool have_pack_ = false;
    bool pack_missed_ = false;
    bool have_minmax_ = false;
    uint32_t frames_ok_ = 0;
    uint32_t timeouts_ = 0;
    uint32_t cmd_timeouts_[kCommandCount] = {};
    uint32_t cmd_crc_[kCommandCount] = {};
    uint32_t crc_seen_ = 0;
    uint8_t live_seen_ = 0;         // bit per live command with an accepted answer
    uint16_t cell_frames_seen_ = 0; // 0x95 frames received for the request in flight
    uint32_t last_timeout_log_ms_ = 0;
    uint32_t sweeps_ = 0;
};
