// SPDX-License-Identifier: Apache-2.0
#include "kernel_bench.hpp"
#include "receiver/bitplane.hpp"
#include "pico/time.h"
#include "hardware/clocks.h"
#include <algorithm>
#include <array>
#include <cstdio>

namespace bench {
namespace {
namespace sig = eph::signal;
namespace bp = sig::bitplane;
bp::Planes from_pins, from_bytes;
std::array<sig::Code, 8> codes;
using Sums = std::array<bp::Correlations, 8>;

uint32_t lane_word(const uint32_t* lane, unsigned first, unsigned count) {
    const unsigned offset = first % 32;
    uint32_t result = lane[first / 32] >> offset;
    if (count > 32 - offset) result |= lane[first / 32 + 1] << (32 - offset);
    return result;
}

void convert_pins(const LaneWords& lanes, unsigned first) {
    from_pins.first_sample = first;
    for (unsigned w = 0; w < bp::words_per_ms; ++w) {
        const unsigned count = std::min(32u, board::samples_per_ms - 32*w);
        const unsigned index = first + 32*w;
        from_pins.words[w] = bp::from_pio(lane_word(lanes[0], index, count),
            lane_word(lanes[1], index, count), lane_word(lanes[2], index, count),
            lane_word(lanes[3], index, count), index, count);
    }
}

bool equal(const bp::Word& a, const bp::Word& b) {
    return a.isign == b.isign && a.imag == b.imag && a.qsign == b.qsign &&
           a.qmag == b.qmag && a.iweight == b.iweight && a.qweight == b.qweight;
}

unsigned time_batch(const bp::Planes& planes, unsigned n, Sums& sums) {
    const auto start = time_us_32();
    for (unsigned i = 0; i < n; ++i)
        sums[i] = bp::correlate(planes, codes[i], 0.375 + i * 37, (int(i)-3)*8500.);
    return time_us_32() - start;
}
}

void time_correlators(const uint32_t* packed, const LaneWords& lanes, bool internal,
                      unsigned capture_us, char* out, size_t capacity) {
    static Sums pin_sums, byte_sums;
    constexpr unsigned counts[] = {1, 4, 8};
    unsigned pin_us[3]{}, byte_us[3]{}, pin_convert = 0, byte_convert = 0, errors = 0;
    for (unsigned i = 0; i < codes.size(); ++i) sig::ca_code(i+1, codes[i]);
    for (unsigned ms = 0; ms < 10; ++ms) {
        const unsigned first = ms * board::samples_per_ms;
        auto start = time_us_32();
        convert_pins(lanes, first);
        pin_convert += time_us_32() - start;
        start = time_us_32();
        bp::from_packed({first, reinterpret_cast<const uint8_t*>(packed) + first/2,
                         sig::bytes_per_ms}, from_bytes);
        byte_convert += time_us_32() - start;
        for (unsigned w = 0; w < bp::words_per_ms; ++w)
            errors += !equal(from_pins.words[w], from_bytes.words[w]);
        for (unsigned j = 0; j < 3; ++j) {
            // Alternate execution order to distribute instruction-cache warm-up costs.
            if (ms % 2) byte_us[j] += time_batch(from_bytes, counts[j], byte_sums);
            pin_us[j] += time_batch(from_pins, counts[j], pin_sums);
            if (!(ms % 2)) byte_us[j] += time_batch(from_bytes, counts[j], byte_sums);
            for (unsigned i = 0; i < counts[j]; ++i) {
                const auto& a = pin_sums[i];
                const auto& b = byte_sums[i];
                errors += a.early.i != b.early.i || a.early.q != b.early.q ||
                          a.prompt.i != b.prompt.i || a.prompt.q != b.prompt.q ||
                          a.late.i != b.late.i || a.late.q != b.late.q;
            }
        }
    }
    std::snprintf(out, capacity,
        "%s samples=%u mismatches=%u clock=%s capture_us=%u epochs=10 "
        "pio_prep_us=%u packed_prep_us=%u pio_1_4_8_us=%u,%u,%u packed_1_4_8_us=%u,%u,%u sys_hz=%u\n",
        errors ? "FAIL" : "PASS", samples, errors, internal ? "internal" : "external", capture_us,
        pin_convert, byte_convert, pin_us[0], pin_us[1], pin_us[2], byte_us[0], byte_us[1], byte_us[2],
        unsigned(clock_get_hz(clk_sys)));
}
}
