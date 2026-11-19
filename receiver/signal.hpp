// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace eph::signal {
namespace bitplane { struct Planes; struct Prefixes; }

inline constexpr int raw_per_ms = 16368;
inline constexpr int bytes_per_ms = raw_per_ms / 2;
inline constexpr int samples_per_ms = raw_per_ms / 4;
inline constexpr int code_length = 1023;
inline constexpr int fft_length = 4096;
inline constexpr double raw_hz = raw_per_ms * 1000.0;
inline constexpr double chip_hz = 1023000.0;
inline constexpr double l1_hz = 1575420000.0;

struct IQ8 { std::int8_t i, q; };
struct IQf { float i, q; };

// Wire format v1: chronological samples in low then high nibble, I1 I0 Q1 Q0 in bits 3..0.
struct PackedBlock {
    std::uint64_t first_sample;
    const std::uint8_t* bytes;
    std::size_t size;  // bytes in this block, exactly bytes_per_ms
    bool discontinuity = false;
};
struct Epoch {
    std::uint64_t first_sample{};
    bool discontinuity{};
    std::array<IQ8, samples_per_ms> samples{}; // four-sample integrate-and-dump; centers at +1.5 raw samples
};

IQ8 decode_nibble(unsigned nibble);
bool preprocess(const PackedBlock& block, Epoch& epoch);
using Code = std::array<std::int8_t, code_length>;
namespace coarse {
struct Epoch;
struct Replica {
    // Periodic extension for an unaligned 34-bit read at the code boundary.
    std::array<std::uint32_t,(2*code_length + 96 + 31)/32> words{};
};
}
bool ca_code(unsigned prn, Code& out); // IS-GPS-200 GPS L1 C/A PRNs 1..32

struct Detection {
    unsigned prn{};
    std::uint64_t first_sample{};
    double code_phase_chips{};  // received code phase at first_sample, increasing with sample time
    float doppler_hz{};         // positive for a carrier above the programmed positive IF
    float metric{};             // coherent peak power / mean code-delay-cell power
    bool detected{};
};

class Acquisition {
public:
    // Freeze one millisecond of data. Bin spacing is 2.046 MHz / 4096 (499.51 Hz).
    // Edges are rounded OUTWARD to bins. A new start discards unfinished work.
    bool start(const Epoch& epoch, unsigned prn, float lo_hz = -70000, float hi_hz = 70000);
    static constexpr unsigned max_dwells = 4;
    bool add_dwell(const Epoch&); // before first step; disjoint epochs within 100 ms
    unsigned dwells() const { return dwells_; }
    bool step(unsigned max_transforms = 1); // at most one inverse FFT per work unit
    Detection best() const { return best_; }
private:
    struct SpectralCell { std::int16_t i, q; };
    std::array<std::array<SpectralCell, fft_length>, max_dwells> data_{};
    std::array<float, max_dwells> weights_{};
    std::array<std::uint64_t, max_dwells> epochs_{};
    std::array<float, samples_per_ms/2> power_{};
    std::array<IQf, fft_length> replica_{}, work_{};
    unsigned dwells_{}, current_dwell_{};
    bool scanning_{};
    int bin_{}, last_bin_{-1};
    Detection best_{};
    unsigned cached_prn_{};
};

// Four fresh dwells refine the candidate. Ambiguous near-far cells receive a
// second, independent check after suppressing the dominant repeated signal.
class Confirmation {
public:
    void start(const Detection& seed);
    bool add(const Epoch& fresh);  // true when verification finishes
    Detection best() const { return best_; }
private:
    Detection seed_{}, best_{};
    Code code_{};
    static constexpr int hypotheses = 3*9; // three Dopplers, nine quarter-chip offsets
    std::array<float, hypotheses> power_{};
    // Compare sparse raw baseband samples at the same point of two code periods.
    static constexpr int repeat_stride = 8;
    std::array<IQ8, (samples_per_ms + repeat_stride - 1)/repeat_stride> previous_{};
    int previous_energy_{};
    float repeat_sum_{};
    unsigned repeat_pairs_{};
    float residual_sum_{};
    unsigned dwells_{};
    std::uint64_t previous_sample_{};
    void measure_repetition(const Epoch& fresh);
    float residual_power(const Epoch& fresh, int offset, float gain_i, float gain_q, int input_energy) const;
};

struct Prompt {
    std::uint64_t epoch{};
    double start_sample{}, end_sample{}; // fractional raw ADC indices at successive code boundaries
    float i{}, q{};                     // unnormalized coherent sums after carrier wipeoff (ADC units)
    int symbol{};                       // +1/-1; Costas polarity ambiguity is resolved by LNAV
};
struct TrackingResult {
    bool active{}, locked{};
    unsigned prn{};
    std::uint64_t first_sample{};
    double code_phase_chips{};
    float doppler_hz{}, prompt_i{}, prompt_q{};
    int symbol{};
    std::uint64_t code_epochs{};
    unsigned count{};
    std::array<Prompt, 2> prompts{};
};

class Tracker {
public:
    // Set once per contiguous input segment, before its first update.
    // Standalone tracking defaults to a report grid beginning at sample zero.
    void set_report_origin(std::uint64_t sample);
    bool start(const Detection& detection);
    TrackingResult update(const Epoch& epoch);
#ifdef EPHEMERIS_COARSE_TRACKING
    TrackingResult update(const coarse::Epoch& epoch);
#endif
    TrackingResult update(const bitplane::Planes& epoch);
    TrackingResult update(const bitplane::Prefixes& epoch);
    void reset() { active_ = false; }
private:
    template<class Input> TrackingResult update_words(const Input&);
    bool begin_update(std::uint64_t first, bool gap, TrackingResult&);
    void end_update(TrackingResult&) const;
    void initialize_phase(std::uint64_t sample);
    void finish_period(double boundary, TrackingResult& result);
    void observe_phase(double sample, double turns, bool good);
    double measured_doppler(double sample) const;
    struct PhaseMean { double sample{}, turns{}; };
    Code code_{};
#ifdef EPHEMERIS_COARSE_TRACKING
    coarse::Replica packed_code_{};
#endif
    Detection seed_{};
    bool active_{}, initialized_{}, full_period_{}, have_previous_{};
    unsigned weak_{}, good_{};
    std::uint64_t next_sample_{}, epoch_number_{};
    double period_start_{}, code_residual_hz_{}, step_remainder_{};
    float doppler_{};
    std::uint32_t carrier_phase_{}, carrier_step_{}, code_phase_{}, code_step_{};
    std::int32_t ei_{}, eq_{}, pi_{}, pq_{}, li_{}, lq_{}, energy_{};
    IQf previous_{};
    unsigned integrated_{}, good_phases_{}, mean_count_{}, smooth_count_{};
    double local_turns_{}, smooth_boundary_{}, phase_sum_{}, time_sum_{};
    std::array<PhaseMean, 3> means_{};
    double fit_sample_{}, fit_hz_{}, fit_hz_per_s_{}, phase_window_end_{};
    bool fit_valid_{}, phase_window_full_{};
    unsigned report_phase_{};
};
} // namespace eph::signal
