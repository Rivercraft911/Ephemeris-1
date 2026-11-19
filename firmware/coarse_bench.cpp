// SPDX-License-Identifier: Apache-2.0
#include "kernel_bench.hpp"
#include "axis_capture.hpp"
#include "receiver/coarse.hpp"
#include "hardware/clocks.h"
#include "pico/time.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace bench {
namespace {
namespace sig = eph::signal;
namespace coarse = sig::coarse;
std::array<coarse::Epoch, 10> reference;
coarse::Epoch from_pins;
std::array<coarse::Replica, 8> replicas;
std::array<coarse::Sums, 8> output;

constexpr uint32_t byte_popcounts(uint32_t x) {
    x -= (x >> 1) & 0x55555555u;
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (x + (x >> 4)) & 0x0f0f0f0fu;
}

// Four eight-sample groups share one 32-bit operation. A signed magnitude
// ADC value is (1 + 2*m)*(1 - 2*s). Its eight-sample sum is negative iff
// pop(s) + 2*pop(s&m) > 4 + pop(m). Byte biases isolate the four comparisons.
constexpr uint32_t four_signs(uint32_t sign, uint32_t magnitude) {
    const uint32_t positive = 0x84848484u + byte_popcounts(magnitude) -
                              byte_popcounts(sign) - 2u*byte_popcounts(sign & magnitude);
    const uint32_t negative = (~positive >> 7) & 0x01010101u;
    return (negative * 0x01020408u) >> 24;
}
static_assert(four_signs(0, 0xffffffffu) == 0 && four_signs(0xffffffffu, 0) == 15);
static_assert(four_signs(0x0f0f0f0fu, 0) == 0);  // Four positive and four negative unit samples tie.
static_assert(four_signs(0x1f0f0703u, 0) == 8);
static_assert(four_signs(0x0f0f0f0fu, 0x0f000f00u) == 10);

template<unsigned Bytes>
inline uint32_t lane_word(const uint32_t* lane, unsigned byte) {
    uint32_t value = 0;
    // Odd 1 ms epochs begin at a halfword offset on the little-endian RP2350.
    std::memcpy(&value, reinterpret_cast<const uint8_t*>(lane) + byte, Bytes);
    return value;
}

template<unsigned Bytes>
inline void append_four(const LaneWords& lanes, unsigned byte, unsigned shift, uint32_t& i, uint32_t& q) {
    const auto mq = lane_word<Bytes>(lanes[0], byte), sq = lane_word<Bytes>(lanes[1], byte);
    const auto mi = lane_word<Bytes>(lanes[2], byte), si = lane_word<Bytes>(lanes[3], byte);
    const auto swap_sign = (si ^ sq) & 0xaaaaaaaau;
    const auto swap_mag = (mi ^ mq) & 0xaaaaaaaau;
    // Capture and every 16,368-sample boundary have the same quarter-IF phase.
    i |= four_signs(si ^ swap_sign ^ 0xccccccccu, mi ^ swap_mag) << shift;
    q |= four_signs(sq ^ swap_sign ^ 0x66666666u, mq ^ swap_mag) << shift;
}

void prepare_pins(const LaneWords& lanes, unsigned first) {
    static_assert(board::samples_per_ms % 32 == 16 && coarse::half_chips % 32 == 30);
    from_pins.first_sample = first;
    unsigned byte = first / 8;
    for (unsigned w = 0; w < coarse::words_per_epoch; ++w) {
        uint32_t i = 0, q = 0;
        // 30 half-chips remain in the last word. Read just the final halfword
        // from each lane so the last epoch never reads beyond the snapshot.
        for (unsigned s = 0; s < 28; s += 4, byte += 4) append_four<4>(lanes, byte, s, i, q);
        if (w+1 == coarse::words_per_epoch) {
            append_four<2>(lanes, byte, 28, i, q);
            i &= 0x3fffffffu;
            q &= 0x3fffffffu;
        } else {
            append_four<4>(lanes, byte, 28, i, q);
        }
        byte += 4;
        from_pins.i[w] = i;
        from_pins.q[w] = q;
    }
}

// Each PIO lane captures one complete two-bit ADC axis. A halfword now holds
// exactly eight successive samples. Store E = x0-x2+x4-x6 and O = x1-x3+x5-x7
// as biased bytes. The Fs/4 mixer outputs E(I)+O(Q) and E(Q)-O(I).
void init_axis_sums(AxisTable& axis_sums) {
    for (unsigned word = 0; word < axis_sums.size(); ++word)
        axis_sums[word] = axis_partial(uint16_t(word));
}

void prepare_pairs(const LaneWords& lanes, unsigned first, const AxisTable& axis_sums) {
    const uint32_t* i_data = lanes.pair(1) + first/16;
    const uint32_t* q_data = lanes.pair(0) + first/16;
    from_pins.first_sample = first;
    for (unsigned w = 0; w < coarse::words_per_epoch; ++w) {
        const unsigned count = std::min(32u, coarse::half_chips - 32*w);
        uint32_t i = 0, q = 0;
        for (unsigned n = 0; n < count; n += 8) {
            unsigned used = std::min(8u, count - n);
            uint32_t bits = 0;
            for (unsigned j = 0; j < used; j += 2) {
                const uint32_t iw = *i_data++, qw = *q_data++;
                const auto a = combine_axes(axis_sums[uint16_t(iw)], axis_sums[uint16_t(qw)]);
                const auto b = combine_axes(axis_sums[iw >> 16], axis_sums[qw >> 16]);
                bits = ((bits >> 2) & 0x3f3fu) | ((a >> 1) & 0x4040u) | (b & 0x8080u);
            }
            const auto negative = ~bits >> (8-used);
            const unsigned valid = (1u << used)-1;
            i |= (negative & valid) << n;
            q |= ((negative >> 8) & valid) << n;
        }
        from_pins.i[w] = i;
        from_pins.q[w] = q;
    }
}
}
void time_coarse(const uint32_t* packed, const LaneWords& lanes, std::byte* workspace,
                 bool pairs, bool internal, char* out, size_t capacity) {
    static sig::Code code;
    for (unsigned i = 0; i < replicas.size(); ++i) {
        sig::ca_code(i+1, code);
        replicas[i] = coarse::ca_replica(code);
    }
    constexpr unsigned counts[] = {1, 4, 8};
    unsigned prep_total = 0, pin_total = 0, corr_total[3]{}, max_total[3]{}, max_pin[3]{}, errors = 0;
    double check = 0;
    unsigned prep_times[10]{};
    for (unsigned ms = 0; ms < 10; ++ms) {
        const unsigned first = ms * board::samples_per_ms;
        auto start = time_us_32();
        errors += !coarse::preprocess({first, reinterpret_cast<const uint8_t*>(packed) + first/2,
                                      sig::bytes_per_ms}, reference[ms]);
        prep_times[ms] = time_us_32() - start;
        prep_total += prep_times[ms];
    }
    // The packed reference is fully drained. Reuse its storage and the wide
    // bitplane scratch for the 128 KiB axis LUT; captured PIO lanes stay live.
    AxisTable* table = nullptr;
    if (pairs) {
        table = ::new (workspace) AxisTable;
        init_axis_sums(*table);
    }
    for (unsigned ms = 0; ms < 10; ++ms) {
        const unsigned first = ms * board::samples_per_ms;
        const auto& input = reference[ms];
        const unsigned prep = prep_times[ms];
        auto start = time_us_32();
        if (pairs) prepare_pairs(lanes, first, *table);
        else prepare_pins(lanes, first);
        const auto pin_prep = time_us_32() - start;
        pin_total += pin_prep;
        errors += input.i != from_pins.i || input.q != from_pins.q;
        for (unsigned k = 0; k < 3; ++k) {
            const unsigned j = (k + ms) % 3;
            start = time_us_32();
            for (unsigned i = 0; i < counts[j]; ++i)
                output[i] = coarse::correlate(input, replicas[i], 0.375 + i*37,
                                              (int(i)-3)*8500., 0.1875);
            const unsigned dt = time_us_32() - start;
            corr_total[j] += dt;
            max_total[j] = std::max(max_total[j], unsigned(prep) + dt);
            max_pin[j] = std::max(max_pin[j], unsigned(pin_prep) + dt);
            // Consume every component outside the timed loop.
            for (unsigned i = 0; i < counts[j]; ++i) {
                const auto& s = output[i];
                for (float value : {s.early.i, s.early.q, s.prompt.i, s.prompt.q, s.late.i, s.late.q}) {
                    errors += !std::isfinite(value) || std::abs(value) > coarse::half_chips ||
                              value != std::trunc(value) || int(value) % 2 != 0;
                    check += value;
                }
            }
        }
    }
    errors += !std::isfinite(check);
    std::snprintf(out, capacity,
        "%s samples=%u errors=%u clock=%s layout=%s sys_hz=%u epochs=10 prep_total_us=%u pio_prep_total_us=%u "
        "corr_1_4_8_total_us=%u,%u,%u worst_prep_corr_us=%u,%u,%u worst_margin_us=%d,%d,%d "
        "worst_pio_margin_us=%d,%d,%d checksum=%.3f\n",
        errors ? "FAIL" : "MEASURED", samples, errors, internal ? "internal" : "external", pairs ? "pairs" : "planes",
        unsigned(clock_get_hz(clk_sys)), prep_total, pin_total, corr_total[0], corr_total[1], corr_total[2],
        max_total[0], max_total[1], max_total[2], 1000-int(max_total[0]), 1000-int(max_total[1]),
        1000-int(max_total[2]), 1000-int(max_pin[0]), 1000-int(max_pin[1]), 1000-int(max_pin[2]), check);
}
}
