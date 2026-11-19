// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace bench {
// One PIO halfword contains eight 2-bit samples from one ADC axis.
constexpr uint16_t axis_partial(uint16_t raw) {
    constexpr int adc[] = {1,3,-1,-3};
    int even = 0, odd = 0;
    for (unsigned pair = 0; pair < 4; ++pair, raw >>= 4) {
        const int sign = (pair & 1) ? -1 : 1;
        even += sign * adc[raw & 3];
        odd += sign * adc[(raw >> 2) & 3];
    }
    return unsigned(129 + even) | (unsigned(uint8_t(-odd)) << 8);
}

inline uint32_t combine_axes(uint32_t i, uint32_t q) {
    // Byte sums become 128+E(I)+O(Q) and 129+E(Q)-O(I). Both axis sums
    // are even, so the sign bit classifies a zero tie identically at either bias.
#if defined(__ARM_FEATURE_DSP)
    asm("rev16 %0,%0" : "+r"(q));
    q ^= 0xffu;
    uint32_t sum;
    asm("uadd8 %0,%1,%2" : "=r"(sum) : "r"(i), "r"(q) : "cc");
    return sum;
#else
    q = __builtin_bswap16(uint16_t(q)) ^ 0xffu;
    return ((i + q) & 0xffu) | (((i & 0xff00u) + (q & 0xff00u)) & 0xff00u);
#endif
}
}
