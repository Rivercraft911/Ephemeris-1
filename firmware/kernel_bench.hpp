// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "board.hpp"
#include "receiver/bitplane.hpp"
#include <cstddef>
#include <cstdint>

namespace bench {
inline constexpr unsigned samples = 10 * board::samples_per_ms;
struct LaneWords {
    uint32_t words[samples/8];
    uint32_t* operator[](unsigned lane) { return words + lane*(samples/32); }
    const uint32_t* operator[](unsigned lane) const { return words + lane*(samples/32); }
    uint32_t* pair(unsigned axis) { return words + axis*(samples/16); }
    const uint32_t* pair(unsigned axis) const { return words + axis*(samples/16); }
};
struct KernelScratch { eph::signal::bitplane::Planes pins, bytes; };
void time_correlators(const uint32_t* packed, const LaneWords& lanes, KernelScratch& scratch, bool internal,
                      unsigned capture_us, char* report, size_t capacity);
using AxisTable = std::array<uint16_t, 65536>;
void time_coarse(const uint32_t* packed, const LaneWords& lanes, std::byte* workspace,
                 bool pairs, bool internal, char* report, size_t capacity);
}
