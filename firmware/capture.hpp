// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "board.hpp"
#include "hardware/pio.h"

namespace capture {
// Right-shift the ISR so time runs from LSB to MSB in the completed 32-bit word.
// A lane width of 1 records one bitplane; width 4 records canonical packed I/Q.
class Lane {
public:
    void init(PIO instance, unsigned machine, unsigned pin, unsigned width, bool bounded);
    void arm(uint32_t* destination, uint32_t word_count, bool circular = false);
    bool stalled() const;
    bool finished() const;
    uint32_t remaining() const;
    void stop();
    void select_bus(unsigned pin, unsigned clock, unsigned width = 0);
private:
    PIO pio_{};
    unsigned sm_{}, dma_{}, offset_{}, width_{}, pin_{};
    bool bounded_{};
};

struct Statistics {
    uint64_t blocks = 0, missed_blocks = 0;
    uint32_t fifo_stalls = 0, clock_timeouts = 0, peak_backlog_bytes = 0;
};

class Stream {
public:
    void start();
    void stop();
    void select_clock(unsigned pin);
    void start_at(unsigned clock_pin, uint64_t first_sample);
    bool read(uint32_t* words, uint64_t& first_sample, bool& gap);
    const Statistics& stats() const { return stats_; }
    bool clock_alive() const { return clock_alive_; }
private:
    Lane lane_;
    Statistics stats_;
    uint64_t base_ = 0, produced_ = 0, consumed_ = 0;
    uint64_t last_poll_us_ = 0, last_progress_us_ = 0;
    uint32_t previous_ = 0;
    unsigned clock_watch_sm_{}, clock_watch_offset_{}, clock_pin_ = board::clock_pin;
    bool clock_alive_ = false, discontinuity_ = true;
    void arm_clock_watch();
    void restart();
    bool update();
};
}
