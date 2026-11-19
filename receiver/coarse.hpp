// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "signal.hpp"

namespace eph::signal::coarse {

inline constexpr unsigned half_chips = 2 * code_length;
inline constexpr unsigned words_per_epoch = (half_chips + 31) / 32;

struct Epoch {
    std::uint64_t first_sample{};
    bool discontinuity{};
    std::array<std::uint32_t, words_per_epoch> i{}, q{};
};

struct Sums { IQf early{}, prompt{}, late{}; };

// Build a replica once per PRN. Negative signs are set bits, earliest at bit 0.
// Correlate a subset of half-chip samples. Both NCO phases refer to the
// first included sample center (3.5 raw clocks after its eight-sample start).
Sums track_interval(const Epoch&, const Replica&, unsigned first, unsigned count,
                    double chips, double chips_per_half, double doppler_hz, double turns);
Replica ca_replica(const Code& code);
bool preprocess(const PackedBlock& block, Epoch& out);
// Phase and positive residual Doppler are specified at the raw block origin.
// Each output axis is a signed sum of 2046 one-bit samples.
Sums correlate(const Epoch& epoch, const Replica& code, double code_phase_chips,
               double doppler_hz, double carrier_turns = 0);
}
