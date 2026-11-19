// SPDX-License-Identifier: Apache-2.0
#include "capture.hpp"
#include <algorithm>
#include <cstring>
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

namespace capture {
namespace {
constexpr unsigned ring_bits = 15;  // DMA ring size limit is 2^15 bytes.
constexpr unsigned ring_words = (1u << ring_bits) / sizeof(uint32_t);
constexpr uint32_t reload = 1u << 27;  // Wraps on a ring boundary, about once per 66 s.
constexpr unsigned clock_fault = 7;    // Local sticky IRQ flag, never routed to the CPU.
alignas(1u << ring_bits) uint32_t ring[ring_words];
constexpr uint32_t cursor_at(uint32_t remaining) { return (reload - remaining) & (reload-1); }
constexpr uint32_t word_delta(uint32_t now, uint32_t previous) { return (now-previous) & (reload-1); }
static_assert(cursor_at(reload) == 0 && cursor_at(0) == 0);
static_assert(word_delta(cursor_at(0), cursor_at(1)) == 1);
static_assert(word_delta(cursor_at(reload-3), cursor_at(2)) == 5);
static_assert(reload % ring_words == 0);
}
void Lane::init(PIO instance, unsigned machine, unsigned pin, unsigned width, bool bounded) {
    pio_ = instance; sm_ = machine; pin_ = pin; width_ = width; bounded_ = bounded;
    pio_sm_claim(pio_, sm_);
    dma_ = dma_claim_unused_channel(true);
    // A complete transition is required even when arming halfway through a pulse.
    const uint16_t instructions[] = {
        uint16_t(pio_encode_wait_gpio(EPHEMERIS_CAPTURE_FALLING, board::clock_pin)),
        uint16_t(pio_encode_wait_gpio(!EPHEMERIS_CAPTURE_FALLING, board::clock_pin)),
        uint16_t(pio_encode_in(pio_pins, width) | (width == 4 ? pio_encode_sideset_opt(1, 1) : 0)),
        uint16_t(pio_encode_jmp_y_dec(0)),
        uint16_t(pio_encode_jmp(4)),  // finite acquisitions end here without stalling the FIFO
    };
    const pio_program_t program{instructions, uint8_t(bounded ? 5 : 3), -1};
    offset_ = pio_add_program(pio_, &program);
    for (unsigned p = pin; p < pin + width; ++p) {
        gpio_init(p); gpio_disable_pulls(p);
    }
    if (width == 4) {
        pio_gpio_init(pio_, board::sample_marker);
        pio_sm_set_consecutive_pindirs(pio_, sm_, board::sample_marker, 1, true);
    }
    gpio_init(board::clock_pin);
    gpio_disable_pulls(board::clock_pin);
    hw_clear_bits(&pio_->input_sync_bypass, ((1u << width) - 1u) << pin | (1u << board::clock_pin));
}

void Lane::select_bus(unsigned pin, unsigned clock, unsigned width) {
    pin_ = pin;
    // Change the two WAIT operands only while the state machine is disabled.
    pio_->instr_mem[offset_] = pio_encode_wait_gpio(EPHEMERIS_CAPTURE_FALLING, clock);
    pio_->instr_mem[offset_ + 1] = pio_encode_wait_gpio(!EPHEMERIS_CAPTURE_FALLING, clock);
    if (width) {
        width_ = width;
        pio_->instr_mem[offset_ + 2] = pio_encode_in(pio_pins, width);
    }
    hw_clear_bits(&pio_->input_sync_bypass, ((1u << width_)-1u) << pin | (1u << clock));
}

void Lane::arm(uint32_t* destination, uint32_t word_count, bool circular) {
    pio_sm_config cfg = pio_get_default_sm_config();
    if (width_ == 4) {
        // One rising edge on the exact first IN of each uninterrupted capture segment.
        pio_sm_set_pins_with_mask(pio_, sm_, 0, 1u << board::sample_marker);
        sm_config_set_sideset(&cfg, 2, true, false);  // enable bit plus one pin
        sm_config_set_sideset_pins(&cfg, board::sample_marker);
    }
    sm_config_set_in_pins(&cfg, pin_);
    sm_config_set_in_shift(&cfg, true, true, 32);
    sm_config_set_wrap(&cfg, offset_, offset_ + (bounded_ ? 4 : 2));
    pio_sm_init(pio_, sm_, offset_, &cfg);
    if (bounded_) {
        pio_sm_put(pio_, sm_, word_count * (32 / width_) - 1);
        pio_sm_exec(pio_, sm_, pio_encode_pull(false, true));
        pio_sm_exec(pio_, sm_, pio_encode_out(pio_y, 32));
    }
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_RX);
    pio_sm_set_config(pio_, sm_, &cfg);
    // FDEBUG is write-one-to-clear. A missed clock invalidates the capture.
    pio_->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + sm_);
    auto dma_cfg = dma_channel_get_default_config(dma_);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_cfg, false);
    channel_config_set_write_increment(&dma_cfg, true);
    channel_config_set_high_priority(&dma_cfg, true);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(pio_, sm_, false));
    if (circular) channel_config_set_ring(&dma_cfg, true, ring_bits);
    const uint32_t count = circular ? dma_encode_transfer_count_with_self_trigger(reload)
                                    : dma_encode_transfer_count(word_count);
    dma_channel_configure(dma_, &dma_cfg, destination, &pio_->rxf[sm_], count, true);
}

bool Lane::stalled() const { return pio_->fdebug & (1u << (PIO_FDEBUG_RXSTALL_LSB + sm_)); }
bool Lane::finished() const { return !dma_channel_is_busy(dma_); }
uint32_t Lane::remaining() const {
    return dma_channel_hw_addr(dma_)->transfer_count & DMA_CH0_TRANS_COUNT_COUNT_BITS;
}
void Lane::stop() {
    pio_sm_set_enabled(pio_, sm_, false);
    dma_channel_abort(dma_);
}

void Stream::start() {
    lane_.init(pio0, 0, board::data_base, 4, false);
    clock_watch_sm_ = pio_claim_unused_sm(pio0, true);
    // After the first rising edge, time each high and low level independently.
    // A pause latches IRQ7 even when the clock recovers before the CPU polls.
    const uint16_t watch[] = {
        uint16_t(pio_encode_wait_gpio(0, clock_pin_)),   // 0: wait for first complete edge
        uint16_t(pio_encode_wait_gpio(1, clock_pin_)),
        uint16_t(pio_encode_mov(pio_x, pio_y)),         // 2: high
        uint16_t(pio_encode_jmp_pin(5)),
        uint16_t(pio_encode_jmp(8)),
        uint16_t(pio_encode_jmp_x_dec(3)),
        uint16_t(pio_encode_irq_set(false, clock_fault)),
        uint16_t(pio_encode_wait_gpio(0, clock_pin_)),
        uint16_t(pio_encode_mov(pio_x, pio_y)),         // 8: low
        uint16_t(pio_encode_jmp_pin(2)),
        uint16_t(pio_encode_jmp_x_dec(9)),
        uint16_t(pio_encode_irq_set(false, clock_fault)),
        uint16_t(pio_encode_wait_gpio(1, clock_pin_)),
    };
    const pio_program_t program{watch, 13, -1};
    clock_watch_offset_ = pio_add_program(pio0, &program);
    restart();
    discontinuity_ = false;  // the first epoch is the acquisition origin, not a sample loss
}
void Stream::stop() {
    lane_.stop();
    pio_sm_set_enabled(pio0, clock_watch_sm_, false);
    clock_alive_ = false;
}
void Stream::select_clock(unsigned pin) {
    stop();
    lane_.select_bus(board::data_base, pin);
    clock_pin_ = pin;
    restart();
}
void Stream::start_at(unsigned pin, uint64_t first_sample) {
    select_clock(pin);
    // A fresh, known bench origin. Subsequent hardware faults are unchanged.
    base_ = first_sample;
    discontinuity_ = false;
}

void Stream::arm_clock_watch() {
    const unsigned offset = clock_watch_offset_;
    for (unsigned i : {0u, 7u}) pio0->instr_mem[offset+i] = pio_encode_wait_gpio(0, clock_pin_);
    for (unsigned i : {1u, 12u}) pio0->instr_mem[offset+i] = pio_encode_wait_gpio(1, clock_pin_);
    auto cfg = pio_get_default_sm_config();
    sm_config_set_jmp_pin(&cfg, clock_pin_);
    sm_config_set_wrap(&cfg, offset + 2, offset + 12);
    pio_sm_init(pio0, clock_watch_sm_, offset, &cfg);
    // The steady-state loop consumes two clk_sys cycles per decrement. At the
    // board's 150 MHz default, 75 iterations plus entry/exit take about 1 us.
    pio_sm_put(pio0, clock_watch_sm_, clock_get_hz(clk_sys) / 2000000u);
    pio_sm_exec(pio0, clock_watch_sm_, pio_encode_pull(false, true));
    pio_sm_exec(pio0, clock_watch_sm_, pio_encode_out(pio_y, 32));
    pio_interrupt_clear(pio0, clock_fault);
}

void Stream::restart() {
    stop();
    // Unknown ADC edges during a hardware stall cannot be reconstructed. Start a
    // new local epoch; no pseudorange may be carried across this discontinuity.
    base_ += ((produced_ + board::words_per_ms - 1) / board::words_per_ms) * board::samples_per_ms;
    produced_ = consumed_ = previous_ = 0;
    discontinuity_ = true;
    clock_alive_ = false;
    last_poll_us_ = last_progress_us_ = time_us_64();
    lane_.arm(ring, ring_words, true);
    arm_clock_watch();
    pio_enable_sm_mask_in_sync(pio0, 1u | (1u << clock_watch_sm_));
}
bool Stream::update() {
    const uint64_t now = time_us_64();
    if (lane_.stalled()) { ++stats_.fifo_stalls; restart(); return false; }
    if (pio_interrupt_get(pio0, clock_fault)) { ++stats_.clock_timeouts; restart(); return false; }
    // The transfer count decrements on a completed write. Normal polling is far
    // faster than one reload; refuse to silently undercount after a long pause.
    if (now - last_poll_us_ > 30000000) { ++stats_.clock_timeouts; restart(); return false; }
    last_poll_us_ = now;
    const uint32_t cursor = cursor_at(lane_.remaining());
    uint32_t delta = word_delta(cursor, previous_);
    previous_ = cursor;
    produced_ += delta;
    if (delta) {
        last_progress_us_ = now;
        clock_alive_ = true;
    } else if (clock_alive_ && now - last_progress_us_ > 4000) {
        ++stats_.clock_timeouts;
        restart();
        return false;
    }
    return true;
}

bool Stream::read(uint32_t* out, uint64_t& first_sample, bool& gap) {
    if (!update()) return false;
    uint64_t available = produced_ - consumed_;
    stats_.peak_backlog_bytes = std::max(stats_.peak_backlog_bytes,
        uint32_t(std::min<uint64_t>(available * 4, UINT32_MAX)));
    if (available < board::words_per_ms) return false;
    if (available > ring_words) {
        // Recover at the newest complete epoch. Discard rather than race a DMA overwrite.
        uint64_t skip = available / board::words_per_ms - 1;
        consumed_ += skip * board::words_per_ms;
        stats_.missed_blocks += skip;
        discontinuity_ = true;
    }
    unsigned offset = consumed_ & (ring_words - 1);
    unsigned first = std::min(board::words_per_ms, ring_words - offset);
    __dmb();
    std::memcpy(out, ring + offset, first * sizeof(uint32_t));
    std::memcpy(out + first, ring, (board::words_per_ms - first) * sizeof(uint32_t));
    __dmb();
    if (!update()) return false;
    if (produced_ - consumed_ > ring_words) {
        // DMA overtook this copy. Never hand a torn block to the receiver.
        ++stats_.missed_blocks;
        consumed_ += board::words_per_ms;
        discontinuity_ = true;
        return false;
    }
    first_sample = base_ + 8 * consumed_;
    consumed_ += board::words_per_ms;
    gap = discontinuity_;
    discontinuity_ = false;
    ++stats_.blocks;
    return true;
}
uint32_t Stream::backlog_bytes() {
    if (!update()) return 0;
    return uint32_t(std::min<uint64_t>((produced_-consumed_)*4, UINT32_MAX));
}
}
