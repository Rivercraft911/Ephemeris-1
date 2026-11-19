// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "signal.hpp"

// Word-parallel code correlation on the four original ADC bits.
namespace eph::signal::bitplane {
inline constexpr unsigned words_per_ms = (raw_per_ms+31)/32;
struct Word {
    std::uint32_t isign, imag, qsign, qmag;
    int iweight, qweight;
};
struct Planes {
    std::uint64_t first_sample{};
    bool discontinuity{};
    std::array<Word,words_per_ms> words;
};
// Each int16 encodes a pair as I + 256*Q. A 32-sample prefix stays within
// ±96 per axis, allowing signed wide sums with no cross-lane information loss.
struct PrefixWord { std::array<std::int16_t,32> iq{}; int energy{}; };
struct Prefixes {
    std::uint64_t first_sample{};
    bool discontinuity{};
    std::array<PrefixWord,words_per_ms> words;
};
struct Correlations { IQf early{}, prompt{}, late{}; int energy{}; };

// Raw PIO lane words arrive in GPIO order Q0,Q1,I0,I1 (earliest sample at bit 0).
Word from_pio(std::uint32_t qmag, std::uint32_t qsign, std::uint32_t imag,
              std::uint32_t isign, std::uint64_t first_sample, unsigned count=32);
bool from_packed(const PackedBlock& block, Planes& out);
bool from_packed(const PackedBlock& block, Prefixes& out);
// Track a piece of one C/A period. Chip and carrier phase refer to its first
// raw sample. Fractional E/L uses deterministic sub-sample mask dithering.
Correlations track_interval(const Planes&, const Code&, unsigned first, unsigned count,
                            double code_phase_chips, double chips_per_sample,
                            double doppler_hz, double carrier_turns);
Correlations track_interval(const Prefixes&, const Code&, unsigned first, unsigned count,
                            double code_phase_chips, double chips_per_sample,
                            double doppler_hz, double carrier_turns);
Correlations correlate(const Planes& data, const Code& code, double phase_chips, double doppler_hz);
} // namespace eph::signal::bitplane
