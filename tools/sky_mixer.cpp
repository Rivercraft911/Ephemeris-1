// SPDX-License-Identifier: Apache-2.0
// Host-only RF fixture generator. The receiver never links this library.
#include <cstddef>
#include <cstdint>

extern "C" void eph_sky_mix(std::size_t samples, const double* seconds,
                            double first_chip, double chip_hz,
                            double carrier_turns, double carrier_hz, double amplitude,
                            const std::int8_t* code, const std::int8_t* navigation,
                            const float* oscillator, float* iq) {
    // Keep the same order of floating-point operations as the NumPy oracle.
    // All coordinates are nonnegative and t is local to one generation span.
    // The independently generated 11-bit IF phase table is interleaved I/Q.
    for (std::size_t n = 0; n < samples; ++n) {
        const double t = seconds[n];
        const auto chip = static_cast<std::uint64_t>(first_chip + chip_hz * t);
        const double a = amplitude * code[chip % 1023] * navigation[chip / 20460];
        const auto phase = static_cast<std::uint64_t>(2048 * (carrier_turns + carrier_hz * t));
        const float* lo = oscillator + 2 * (phase & 2047);
        iq[2 * n] = static_cast<float>(iq[2 * n] + a * lo[0]);
        iq[2 * n + 1] = static_cast<float>(iq[2 * n + 1] + a * lo[1]);
    }
}
