// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace board {
// Increasing GPIO significance mirrors the nibble [I sign, I mag, Q sign, Q mag].
constexpr unsigned data_base = 2, clock_pin = 6, sample_marker = 7;
constexpr unsigned select_pin = 17, sclk_pin = 18, sdata_pin = 19;
constexpr uint32_t sample_hz = 16368000;
constexpr int32_t if_hz = 4092000;
constexpr unsigned samples_per_ms = sample_hz / 1000;
constexpr unsigned words_per_ms = samples_per_ms / 8;
static_assert(samples_per_ms % 8 == 0);
void configure_frontend();
}
