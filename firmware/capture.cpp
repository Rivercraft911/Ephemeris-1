// SPDX-License-Identifier: Apache-2.0
#include "capture.hpp"
#include <algorithm>
#include <cstring>
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

namespace capture {
namespace {
constexpr unsigned ring_bits = 15;  // DMA ring size limit is 2^15 bytes.
constexpr unsigned ring_words = (1u << ring_bits) / sizeof(uint32_t);
constexpr uint32_t reload = 1u << 27;  // Wraps on a ring boundary, about once per 66 s.
alignas(1u << ring_bits) uint32_t ring[ring_words];
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

void Lane::select_bus(unsigned pin, unsigned clock) {
    pin_ = pin;
    // Change the two WAIT operands only while the state machine is disabled.
    pio_->instr_mem[offset_] = pio_encode_wait_gpio(EPHEMERIS_CAPTURE_FALLING, clock);
    pio_->instr_mem[offset_ + 1] = pio_encode_wait_gpio(!EPHEMERIS_CAPTURE_FALLING, clock);
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
    restart();
    discontinuity_ = false;  // the first epoch is the acquisition origin, not a sample loss
}
void Stream::select_clock(unsigned pin) {
    lane_.stop();
    lane_.select_bus(board::data_base, pin);
    restart();
}
void Stream::restart() {
    lane_.stop();
    // Unknown ADC edges during a hardware stall cannot be reconstructed. Start a
    // new local epoch; no pseudorange may be carried across this discontinuity.
    base_ += ((produced_ + board::words_per_ms - 1) / board::words_per_ms) * board::samples_per_ms;
    produced_ = consumed_ = previous_ = 0;
    discontinuity_ = true;
    clock_alive_ = false;
    last_poll_us_ = last_progress_us_ = time_us_64();
    lane_.arm(ring, ring_words, true);
    pio_sm_set_enabled(pio0, 0, true);
}
bool Stream::update() {
    const uint64_t now = time_us_64();
    if (lane_.stalled()) { ++stats_.fifo_stalls; restart(); return false; }
    // The transfer count decrements on a completed write. Normal polling is far
    // faster than one reload; refuse to silently undercount after a long pause.
    if (now - last_poll_us_ > 30000000) { ++stats_.clock_timeouts; restart(); return false; }
    last_poll_us_ = now;
    const uint32_t cursor = (reload - lane_.remaining()) & (reload - 1);
    uint32_t delta = (cursor - previous_) & (reload - 1);
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
}
