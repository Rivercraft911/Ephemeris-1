// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace replay_crc {
constexpr auto make_table() {
    std::array<uint32_t, 256> values{};
    for (uint32_t i = 0; i < values.size(); ++i) {
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
        values[i] = crc;
    }
    return values;
}
inline constexpr auto table = make_table();
inline uint32_t update(uint32_t crc, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 255];
    return crc;
}
}
