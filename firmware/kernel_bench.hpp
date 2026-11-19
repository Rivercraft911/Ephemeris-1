// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "board.hpp"
#include <cstddef>
#include <cstdint>

namespace bench {
inline constexpr unsigned samples = 10 * board::samples_per_ms;
using LaneWords = uint32_t[4][samples / 32];
void time_correlators(const uint32_t* packed, const LaneWords& lanes, bool internal,
                      unsigned capture_us, char* report, size_t capacity);
void time_coarse(const uint32_t* packed, bool internal, char* report, size_t capacity);
}
