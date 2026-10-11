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
    static constexpr uint8_t kCommandCount = 8;
    static constexpr uint8_t kLiveCommandCount = 6;

    // `serial` must already be begin()-ed at 9600 8N1.
    void begin(Stream* serial, uint32_t poll_interval_ms, uint32_t stale_after_ms);

    // Changes the cadence without disturbing a sweep that is under way.
    void setPolling(uint32_t poll_interval_ms, uint32_t stale_after_ms);

    void loop();

    // True while a request is out and its reply has not been collected. The CPU
    // must stay awake then: SoftwareSerial receives by interrupt and a light-sleeping
    // core would miss the reply.
    bool busy() const { return state_ == State::Waiting; }

    // Milliseconds until the next sweep starts, 0 if it is due (or under way).
    uint32_t msUntilSweep() const;

    const daly::BmsData& data() const { return data_; }

    // True once we have had at least one good frame and it is recent enough.
    bool valid() const;

    // Seconds since the last checksum-valid frame, saturating at 65535.
    // 65535 also means "never had one".
    uint16_t secondsSinceGoodFrame() const;

    uint32_t framesOk() const { return frames_ok_; }
    uint32_t timeouts() const { return timeouts_; }
    uint32_t checksumErrors() const { return decoder_.checksumErrors(); }
    uint32_t sweeps() const { return sweeps_; }

private:
    enum class State : uint8_t { Idle, Waiting };

    // Older firmware may not implement 0x59/0x5A. Give up after a few sweeps so
    // it does not cost two timeouts and a log line on every one.
    static constexpr uint8_t kMaxLimitSweeps = 3;
    bool needsLimits() const {
        return (data_.cell_low_l1_mv == 0 || data_.pack_low_l1_dv == 0) &&
               limit_sweeps_ < kMaxLimitSweeps;
    }
    uint8_t limit_sweeps_ = 0;
    void sendCurrentCommand();
    void advance();

    // The BMS normally answers within ~30 ms. 0x95 streams several frames back
    // to back, and we deliberately wait out the whole window to collect them.
    static constexpr uint32_t kResponseTimeoutMs = 250;

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
    uint32_t frames_ok_ = 0;
    uint32_t timeouts_ = 0;
    uint32_t sweeps_ = 0;
};
