// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace eph::nav {

constexpr double speed_of_light = 299792458.0;
constexpr double l1_frequency = 1575420000.0;
constexpr double sample_rate = 16368000.0;
constexpr double week_seconds = 604800.0;
using Vec3 = std::array<double, 3>;

// GPS broadcast units converted to SI. Angles and angular rates use radians.
// 'week_mod1024' refers to the subframe 1 transmission week. The ephemeris
// epochs toe and toc may be in the adjacent week near a rollover.
struct Ephemeris {
    int week_mod1024 = -1, iodc = -1, health = 63, ura = 15;
    double toe = 0, toc = 0, sqrt_a = 0, ecc = 0;
    double m0 = 0, delta_n = 0, omega0 = 0, i0 = 0, omega = 0;
    double omega_dot = 0, idot = 0;
    double cuc = 0, cus = 0, cic = 0, cis = 0, crc = 0, crs = 0;
    double af0 = 0, af1 = 0, af2 = 0, tgd = 0;
    double sf1_tow = 0;
};

struct Satellite {
    Vec3 position_m{}, velocity_mps{};
    double clock_s = 0, clock_drift = 0; // includes relativity and L1 group delay
};

// t is GPS transmit time, in seconds of week. Healthy LNAV with a fresh
// toe/toc is required. The conservative four-hour fit also covers longer fits.
bool usable_at(const Ephemeris& ephemeris, double t);
bool propagate(const Ephemeris& ephemeris, double t, Satellite& sat);

struct Ionosphere {
    // Signed LNAV alpha0..3 and beta0..3 in their transmitted scale.
    std::array<int8_t, 8> coefficients{};
    double received_sample = -1;
    bool fresh(double sample) const {
        return received_sample >= 0 && sample >= received_sample &&
               sample - received_sample < 86400 * sample_rate;
    }
};

class LnavDecoder {
public:
    // Feed contiguous prompt integrations, each aligned to one full C/A code
    // period. Sample timestamps refer to its first chip and can be fractional.
    void push_prompt(double prompt_i, double code_epoch_sample);
    // Alternate input for replay of hard 50 bit/s symbols, MSB first.
    void push_bit(bool bit, double bit_start_sample);
    void reset() { *this = {}; }
    const Ephemeris* ephemeris() const { return ready_ ? &active_ : nullptr; }
    // Interpolates arrival time across tracked code epochs. Accepts a common
    // fractional ADC sample epoch and returns the satellite-clock transmit TOW.
    bool transmit_time(double sample, double& sv_tow_s) const;
    bool can_use_cached(const Ephemeris& saved) const;
    // The default health field is 63 until the first parity-checked SF1 arrives.
    bool broadcast_unhealthy() const { return received_[0] >= 0 && pending_.health != 0; }
    unsigned good_subframes() const { return good_; }
#ifdef EPHEMERIS_GROUND_IONOSPHERE
    const Ionosphere& ionosphere() const { return ionosphere_; }
#endif

private:
    void accept(const std::array<uint32_t, 10>& data, double bit_start);
    std::array<uint8_t, 300> bits_{};
    std::array<unsigned, 20> votes_{};
    unsigned head_ = 0, used_ = 0, epoch_mod20_ = 0, good_ = 0, sum_count_ = 0;
    int phase_ = -1;
    bool prev_sign_ = false, timed_ = false, ready_ = false;
    double prev_epoch_sample_ = -1, period_samples_ = sample_rate / 1000;
    double sum_ = 0, sum_start_ = 0, bit_start_ = -1;
    int bit_tow_ms_ = 0;
    double verified_sample_ = -1;
    // Complete sets are promoted atomically. The pending issue checks also
    // work for SF2/SF3 received before SF1.
    Ephemeris pending_{}, active_{};
    int issue2_ = -1, issue3_ = -1;
    std::array<double, 3> received_{{-1, -1, -1}};
#ifdef EPHEMERIS_GROUND_IONOSPHERE
    Ionosphere ionosphere_{};
#endif
};

struct Observation {
    const Ephemeris* ephemeris = nullptr;
    double sv_tow_s = 0;    // SV-clock transmission time at common receive epoch
    double doppler_hz = 0;  // positive when the measured carrier frequency rises
    unsigned prn = 0;       // 1..32 for live observations
};

struct Fix {
    Vec3 position_m{}, velocity_mps{};
    double tow_s = 0, clock_bias_m = 0, clock_drift_mps = 0;
    double pdop = 0, range_rms_m = 0, rate_rms_mps = 0;
    int week_mod1024 = -1;
    unsigned satellites = 0;
    bool valid = false;
};

// Combine successive valid 5 Hz solutions. Velocity and clock remain the
// single-epoch estimates. sample is the receiver's continuous raw ADC counter.
class PositionSmoother {
public:
    static constexpr uint64_t interval_samples = static_cast<uint64_t>(sample_rate) / 5;
    void update(Fix& fix, uint64_t sample);
private:
    Vec3 position_{}, velocity_{};
    uint64_t previous_sample_{};
    bool initialized_{};
};

// Mean of consecutive code positions transported to the newest reported epoch
// by trapezoidal integration of the reported ECEF velocities (5 Hz).
class MotionAverage {
public:
#ifdef EPHEMERIS_MOTION_WINDOW
    static constexpr unsigned window = EPHEMERIS_MOTION_WINDOW;
#else
    static constexpr unsigned window = 40;
#endif
    static_assert(window >= 2 && window <= 100);
    unsigned update(Fix& fix, uint64_t sample); // number of contributing fixes
    void reset() { count_ = 0; }
private:
    std::array<std::array<float, 3>, window-1> innovations_{};
    Vec3 previous_position_{}, previous_velocity_{};
    uint64_t previous_sample_{};
    unsigned next_{}, count_{};
};

// Five-sample causal least-squares line evaluated at the newest Doppler epoch.
class VelocityFit {
public:
    void update(Fix& fix, uint64_t sample);
private:
    std::array<Vec3, 4> history_{};
    uint64_t previous_sample_{};
    unsigned count_{};
};

// Supply TOW from a free-running local sample-clock origin to retain a
// meaningful receiver bias over successive fixes. Without one, the initial
// receiver epoch is assigned 75 ms after the first transmitter's timestamp.
// Failure clears the output, including its validity flag.
bool solve_fix(const Observation* obs, std::size_t n, Fix& fix,
               double receiver_tow_s = std::numeric_limits<double>::quiet_NaN(),
               const Ionosphere* ionosphere = nullptr);

} // namespace eph::nav
