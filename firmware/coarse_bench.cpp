// SPDX-License-Identifier: Apache-2.0
#include "kernel_bench.hpp"
#include "receiver/coarse.hpp"
#include "hardware/clocks.h"
#include "pico/time.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace bench {
namespace {
namespace sig = eph::signal;
namespace coarse = sig::coarse;
coarse::Epoch input;
std::array<coarse::Replica, 8> replicas;
std::array<coarse::Sums, 8> output;
}
void time_coarse(const uint32_t* packed, bool internal, char* out, size_t capacity) {
    static sig::Code code;
    for (unsigned i = 0; i < replicas.size(); ++i) {
        sig::ca_code(i+1, code);
        replicas[i] = coarse::ca_replica(code);
    }
    constexpr unsigned counts[] = {1, 4, 8};
    unsigned prep_total = 0, corr_total[3]{}, max_total[3]{}, errors = 0;
    double check = 0;
    for (unsigned ms = 0; ms < 10; ++ms) {
        const unsigned first = ms * board::samples_per_ms;
        auto start = time_us_32();
        errors += !coarse::preprocess({first, reinterpret_cast<const uint8_t*>(packed) + first/2,
                                      sig::bytes_per_ms}, input);
        const auto prep = time_us_32() - start;
        prep_total += prep;
        for (unsigned k = 0; k < 3; ++k) {
            const unsigned j = (k + ms) % 3;
            start = time_us_32();
            for (unsigned i = 0; i < counts[j]; ++i)
                output[i] = coarse::correlate(input, replicas[i], 0.375 + i*37,
                                              (int(i)-3)*8500., 0.1875);
            const unsigned dt = time_us_32() - start;
            corr_total[j] += dt;
            max_total[j] = std::max(max_total[j], unsigned(prep) + dt);
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
        "%s samples=%u errors=%u clock=%s sys_hz=%u epochs=10 prep_total_us=%u "
        "corr_1_4_8_total_us=%u,%u,%u worst_prep_corr_us=%u,%u,%u worst_margin_us=%d,%d,%d checksum=%.3f\n",
        errors ? "FAIL" : "MEASURED", samples, errors, internal ? "internal" : "external",
        unsigned(clock_get_hz(clk_sys)), prep_total, corr_total[0], corr_total[1], corr_total[2],
        max_total[0], max_total[1], max_total[2], 1000-int(max_total[0]), 1000-int(max_total[1]),
        1000-int(max_total[2]), check);
}
}
