// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>

namespace board {
// MAX2769B, Table 2 and registers 0..9. docs/design.md explains these 28-bit values.
inline constexpr std::array<uint32_t, 10> register_data = {
    0xA295163, 0x855028C, 0xEAFF1DC, 0x9EC0008, 0x0C00080,
    0x8000070, 0x8000000, 0x10061B2, 0x1E0F401, 0x28C0402,
};
constexpr std::array<uint8_t, 4> configuration_bytes(unsigned address) {
    const uint32_t word = (register_data[address] << 4) | address;
    return {uint8_t(word >> 24), uint8_t(word >> 16), uint8_t(word >> 8), uint8_t(word)};
}
}
