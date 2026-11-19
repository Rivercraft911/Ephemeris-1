// SPDX-License-Identifier: Apache-2.0
#include "navigation.hpp"
#if defined(EPHEMERIS_GROUND_TROPOSPHERE) || defined(EPHEMERIS_GROUND_IONOSPHERE)
#include "troposphere.hpp"
#endif

#include <algorithm>
#include <cmath>

namespace eph::nav {
namespace {
constexpr double pi = 3.1415926535897932384626433832795;
constexpr double earth_rate = 7.2921151467e-5;
constexpr double gm = 3.986005e14;

double week_time(double t) {
    t = std::fmod(t, week_seconds);
    return t < 0 ? t + week_seconds : t;
}
double week_diff(double a, double b) { return std::remainder(a - b, week_seconds); }

// ICD bit numbering is 1-based and starts at the most significant data bit.
uint32_t field(uint32_t word, int bit, int length) {
    return (word >> (25 - bit - length)) & ((1u << length) - 1u);
}
double signed_scaled(uint32_t raw, int length, int exp) {
    const int64_t value = (raw >> (length - 1)) ? int64_t(raw) - (int64_t{1} << length) : raw;
    return std::ldexp(static_cast<double>(value), exp);
}
uint32_t wide(uint32_t high_word, uint32_t low_word) {
    return ((high_word & 0xffu) << 24) | low_word;
}

// IS-GPS-200N, Table 20-XIV. The two least significant bits of HOW and word
// 10 are always zero after transmission, giving a known parity history at TLM.
bool decode_word(uint32_t wire, unsigned history, uint32_t& data) {
    constexpr uint32_t masks[] = {0xec7cd2, 0x763e69, 0xbb1f34, 0x5d8f9a, 0xaec7cd, 0x2dea27};
    constexpr int carry[] = {1, 0, 1, 0, 0, 1};
    data = (wire >> 6) ^ ((history & 1u) ? 0xffffffu : 0u);
    unsigned parity = 0;
    for (unsigned i = 0; i < 6; ++i) {
        const unsigned h = (history >> carry[i]) & 1;
        parity = (parity << 1) | (static_cast<unsigned>(__builtin_parity(data & masks[i])) ^ h);
    }
    return parity == (wire & 63u);
}

double dot(const Vec3& a, const Vec3& b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
Vec3 subtract(const Vec3& a, const Vec3& b) {
    return {a[0]-b[0], a[1]-b[1], a[2]-b[2]};
}
Vec3 reception_axes(const Vec3& transmitted, double travel) {
    const double a = earth_rate * travel;
    const double c = std::cos(a), s = std::sin(a);
    return {c*transmitted[0] + s*transmitted[1], -s*transmitted[0] + c*transmitted[1], transmitted[2]};
}

struct LeastSquares {
    // Four unknowns, at most 32 rows. Accumulate symmetric normal equations
    // in double and use the inverse diagonal for PDOP and a geometry veto.
    double matrix[4][4]{}, rhs[4]{}, sumsq = 0;
    void add(Vec3 direction, double residual) {
        const double row[4] = {direction[0], direction[1], direction[2], 1.};
        for (int i = 0; i < 4; ++i) {
            rhs[i] += row[i] * residual;
            for (int j = 0; j < 4; ++j) matrix[i][j] += row[i] * row[j];
        }
        sumsq += residual * residual;
    }
    bool solve(std::array<double,4>& correction, double& pdop) const {
        double a[4][8]{};
        for (int i = 0; i < 4; ++i) {
            std::copy_n(matrix[i], 4, a[i]);
            a[i][i+4] = 1;
        }
        for (int i = 0; i < 4; ++i) {
            int pivot = i;
            for (int j = i+1; j < 4; ++j)
                if (std::abs(a[j][i]) > std::abs(a[pivot][i])) pivot = j;
            if (std::abs(a[pivot][i]) < 1e-10) return false;
            if (pivot != i) std::swap(a[pivot], a[i]);
            const double divisor = a[i][i];
            for (double& x : a[i]) x /= divisor;
            for (int j = 0; j < 4; ++j) {
                if (j == i) continue;
                const double multiple = a[j][i];
                for (int k = 0; k < 8; ++k) a[j][k] -= multiple * a[i][k];
            }
        }
        pdop = std::sqrt(a[0][4] + a[1][5] + a[2][6]);
        if (!(pdop <= 50)) return false;
        for (int i = 0; i < 4; ++i) {
            correction[i] = 0;
            for (int j = 0; j < 4; ++j) correction[i] += a[i][4+j]*rhs[j];
        }
        return true;
    }
};
} // namespace

void LnavDecoder::push_prompt(double prompt_i, double epoch_sample) {
    const double dt = epoch_sample - prev_epoch_sample_;
    if (!std::isfinite(epoch_sample) || !std::isfinite(prompt_i) || epoch_sample < 0) {
        reset();
        return;
    }
    if (prev_epoch_sample_ >= 0 && (dt < 0.998*sample_rate/1000 || dt > 1.002*sample_rate/1000))
        reset();
    const bool sign = prompt_i < 0;
    if (prev_epoch_sample_ >= 0) {
        period_samples_ += (dt - period_samples_) / 16;
        // Bit transitions can only occur at a data boundary. Voting suppresses
        // isolated sign errors before coherent 20-period integration begins.
        if (sign != prev_sign_) {
            auto& vote = votes_[epoch_mod20_];
            ++vote;
            if (vote >= 64)
                for (auto& v : votes_) v /= 2;
            unsigned runner_up = 0;
            for (unsigned i = 0; i < votes_.size(); ++i)
                if (i != epoch_mod20_) runner_up = std::max(runner_up, votes_[i]);
            if (vote >= 6 && vote > 2*runner_up && phase_ != static_cast<int>(epoch_mod20_)) {
                phase_ = static_cast<int>(epoch_mod20_);
                head_ = used_ = sum_count_ = 0;
                bit_start_ = -1;
                timed_ = false;
            }
        }
    }
    prev_sign_ = sign;
    prev_epoch_sample_ = epoch_sample;
    if (phase_ >= 0) {
        if (phase_ == static_cast<int>(epoch_mod20_)) {
            sum_ = 0;
            sum_count_ = 0;
            sum_start_ = epoch_sample;
        }
        sum_ += prompt_i;
        if (++sum_count_ == 20) push_bit(sum_ < 0, sum_start_);
    }
    epoch_mod20_ = (epoch_mod20_ + 1) % 20;
}

void LnavDecoder::push_bit(bool bit, double start) {
    if (!std::isfinite(start) || start < 0) { reset(); return; }
    if (bit_start_ >= 0) {
        const double measured = (start - bit_start_) / 20;
        if (std::abs(measured - sample_rate/1000) > 0.002*sample_rate/1000) {
            reset();
        } else {
            period_samples_ = measured;
            if (timed_) bit_tow_ms_ = (bit_tow_ms_ + 20) % 604800000;
        }
    }
    bit_start_ = start;
    bits_[head_] = bit;
    head_ = (head_ + 1) % 300;
    if (used_ < 300 && ++used_ < 300) return;

    uint8_t preamble = 0;
    for (unsigned i = 0; i < 8; ++i)
        preamble = static_cast<uint8_t>(preamble*2 + bits_[(head_+i)%300]);
    if (preamble != 0x8b && preamble != 0x74) return;
    const unsigned flip = preamble == 0x74;
    unsigned history = 0, at = head_;
    std::array<uint32_t,10> words{};
    for (int j = 0; j < 10; ++j) {
        uint32_t wire = 0;
        for (int k = 0; k < 30; ++k) {
            wire = (wire << 1) | (bits_[at] ^ flip);
            if (++at == 300) at = 0;
        }
        if (!decode_word(wire, history, words[j])) return;
        history = wire & 3u;
        if ((j == 1 || j == 9) && history != 0) return;
    }
    accept(words, start);
}

void LnavDecoder::accept(const std::array<uint32_t,10>& w, double start) {
    const unsigned z = field(w[1], 1, 17), sf = field(w[1], 20, 3);
    if (z >= 100800 || sf < 1 || sf > 5 || (z % 5) != (sf % 5)) return;
    if (field(w[1], 18, 1)) {
        timed_ = ready_ = false;
        pending_ = {}; issue2_ = issue3_ = -1; received_.fill(-1);
#ifdef EPHEMERIS_GROUND_IONOSPHERE
        ionosphere_ = {};
#endif
        return;
    }
    const int current = (int(z)*6000 + 604800000 - 20) % 604800000;
    if (timed_ && bit_tow_ms_ != current) {
        pending_ = {}; issue2_ = issue3_ = -1; received_.fill(-1);
        ready_ = false;
#ifdef EPHEMERIS_GROUND_IONOSPHERE
        ionosphere_ = {};
#endif
    }
    bit_tow_ms_ = current;
    verified_sample_ = start;
    timed_ = true;
    ++good_;
#ifdef EPHEMERIS_GROUND_IONOSPHERE
    if (sf == 4 && field(w[2],1,2) == 1 && field(w[2],3,6) == 56) {
        // Page 18, IS-GPS-200N Table 20-X. All ten words passed parity.
        for (unsigned i = 0; i < 8; ++i) {
            const unsigned byte = i + 1;
            const int raw = int(field(w[2 + byte/3], 1 + 8*(byte%3), 8));
            ionosphere_.coefficients[i] = int8_t(raw < 128 ? raw : raw-256);
        }
        ionosphere_.received_sample = start;
    }
#endif
    if (sf > 3) return;
    auto& e = pending_;
    if (sf == 1) {
        e.sf1_tow = week_time(z*6.0 - 6.);
        e.week_mod1024 = int(field(w[2],1,10));
        e.ura = int(field(w[2],13,4));
        e.health = int(field(w[2],17,6));
        e.iodc = int((field(w[2],23,2) << 8) | field(w[7],1,8));
        e.tgd = signed_scaled(field(w[6],17,8),8,-31);
        e.toc = field(w[7],9,16)*16.;
        e.af2 = signed_scaled(field(w[8],1,8),8,-55);
        e.af1 = signed_scaled(field(w[8],9,16),16,-43);
        e.af0 = signed_scaled(field(w[9],1,22),22,-31);
    } else if (sf == 2) {
        issue2_ = int(field(w[2],1,8));
        e.crs = signed_scaled(field(w[2],9,16),16,-5);
        e.delta_n = pi*signed_scaled(field(w[3],1,16),16,-43);
        e.m0 = pi*signed_scaled(wide(w[3],w[4]),32,-31);
        e.cuc = signed_scaled(field(w[5],1,16),16,-29);
        e.ecc = std::ldexp(double(wide(w[5],w[6])),-33);
        e.cus = signed_scaled(field(w[7],1,16),16,-29);
        e.sqrt_a = std::ldexp(double(wide(w[7],w[8])),-19);
        e.toe = field(w[9],1,16)*16.;
    } else {
        e.cic = signed_scaled(field(w[2],1,16),16,-29);
        e.omega0 = pi*signed_scaled(wide(w[2],w[3]),32,-31);
        e.cis = signed_scaled(field(w[4],1,16),16,-29);
        e.i0 = pi*signed_scaled(wide(w[4],w[5]),32,-31);
        e.crc = signed_scaled(field(w[6],1,16),16,-5);
        e.omega = pi*signed_scaled(wide(w[6],w[7]),32,-31);
        e.omega_dot = pi*signed_scaled(w[8],24,-43);
        issue3_ = int(field(w[9],1,8));
        e.idot = pi*signed_scaled(field(w[9],9,14),14,-43);
    }
    received_[sf-1] = start;
    // A newly broadcast issue or health flag must not produce a fix by
    // joining a fresh clock to an orbit from the preceding issue.
    ready_ = false;
    if (e.iodc < 0 || e.health || e.ura == 15 || issue2_ != issue3_ || (e.iodc & 255) != issue2_) return;
    for (const double received : received_)
        if (received < 0 || start - received > 90*sample_rate) return;
    active_ = e;
    ready_ = true;
}

bool LnavDecoder::can_use_cached(const Ephemeris& saved) const {
    if (!timed_ || pending_.iodc < 0 || saved.iodc != pending_.iodc ||
        pending_.health || pending_.ura >= 15 || saved.health || saved.ura >= 15 ||
        pending_.toc != saved.toc || (issue2_ >= 0 && pending_.toe != saved.toe)) return false;
    const double elapsed = week_diff(pending_.sf1_tow, saved.sf1_tow);
    const int candidate_week = (saved.week_mod1024 +
        int(std::floor((saved.sf1_tow + elapsed)/week_seconds)) + 1024) % 1024;
    if (candidate_week != pending_.week_mod1024 || std::abs(elapsed) > 7200 ||
        std::abs(week_diff(pending_.sf1_tow,saved.toe)) > 7200) return false;
    return (issue2_ < 0 || issue2_ == (saved.iodc & 255)) &&
           (issue3_ < 0 || issue3_ == (saved.iodc & 255));
}

bool LnavDecoder::transmit_time(double sample, double& sv_tow_s) const {
    if (!timed_ || !std::isfinite(sample) ||
        sample - verified_sample_ > 18.1 * sample_rate ||
        std::abs(sample-bit_start_) > .045*sample_rate) return false;
    sv_tow_s = week_time(bit_tow_ms_*.001 + (sample-bit_start_) * .001 / period_samples_);
    return true;
}

bool usable_at(const Ephemeris& e, double t) {
    if (e.week_mod1024 < 0 || e.week_mod1024 > 1023 || e.iodc < 0 || e.health ||
        e.ura >= 15 || !(e.sqrt_a >= 2530 && e.sqrt_a <= 8192) ||
        !(e.ecc >= 0 && e.ecc <= .03) || !(e.toe >= 0 && e.toe < week_seconds) ||
        !(e.toc >= 0 && e.toc < week_seconds) || !std::isfinite(t)) return false;
    return std::abs(week_diff(t, e.toe)) <= 7200 && std::abs(week_diff(t, e.toc)) <= 7200;
}

bool propagate(const Ephemeris& e, double t, Satellite& sat) {
    // IS-GPS-200N Tables 20-IV and 20-V, including the eccentric-orbit
    // relativistic correction in the transmitted satellite clock.
    if (!usable_at(e, t)) return false;
    const double tk = week_diff(t, e.toe), tc = week_diff(t, e.toc);
    const double a = e.sqrt_a*e.sqrt_a, n = std::sqrt(gm/(a*a*a)) + e.delta_n;
    const double m = e.m0 + n*tk;
    double E = m;
    for (int i = 0; i < 4; ++i) E -= (E - e.ecc*std::sin(E) - m)/(1-e.ecc*std::cos(E));
    const double se = std::sin(E), ce = std::cos(E), den = 1 - e.ecc*ce;
    const double root = std::sqrt(1-e.ecc*e.ecc);
    const double phi = std::atan2(root*se, ce-e.ecc) + e.omega;
    const double s2 = std::sin(2*phi), c2 = std::cos(2*phi);
    const double u = phi + e.cus*s2 + e.cuc*c2;
    const double r = a*den + e.crs*s2 + e.crc*c2;
    const double inc = e.i0 + e.idot*tk + e.cis*s2 + e.cic*c2;
    const double od = e.omega_dot - earth_rate;
    const double node = e.omega0 + od*tk - earth_rate*e.toe;
    const double su = std::sin(u), cu = std::cos(u);
    const double si = std::sin(inc), ci = std::cos(inc);
    const double so = std::sin(node), co = std::cos(node);
    const double x = r*cu, y = r*su;
    sat.position_m = {x*co-y*ci*so, x*so+y*ci*co, y*si};
    const double ed = n/den, phid = root*ed/den;
    const double ud = phid*(1 + 2*(e.cus*c2 - e.cuc*s2));
    const double rd = a*e.ecc*se*ed + 2*phid*(e.crs*c2 - e.crc*s2);
    const double id = e.idot + 2*phid*(e.cis*c2 - e.cic*s2);
    const double xd = rd*cu - r*su*ud, yd = rd*su + r*cu*ud;
    const double ycd = yd*ci - y*si*id;
    sat.velocity_mps = {xd*co-ycd*so - sat.position_m[1]*od,
                        xd*so+ycd*co + sat.position_m[0]*od, yd*si + y*ci*id};
    constexpr double F = -4.442807633e-10;
    const double relativity = F * e.ecc * e.sqrt_a;
    sat.clock_s = e.af0 + tc*(e.af1 + tc*e.af2) + relativity*se - e.tgd;
    sat.clock_drift = e.af1 + 2*tc*e.af2 + relativity*ce*ed;
    return std::isfinite(sat.clock_s + sat.clock_drift + dot(sat.position_m,sat.velocity_mps));
}

void PositionSmoother::update(Fix& fix, uint64_t sample) {
    if (!fix.valid) {
        initialized_ = false;
        return;
    }
    if (initialized_ && sample > previous_sample_ && sample - previous_sample_ == interval_samples) {
        constexpr double dt = interval_samples / sample_rate;
        constexpr double gain = dt / 8.0;
        for (unsigned i = 0; i < position_.size(); ++i) {
            const double predicted = position_[i] + 0.5 * dt * (velocity_[i] + fix.velocity_mps[i]);
            fix.position_m[i] = predicted + gain * (fix.position_m[i] - predicted);
        }
    }
    position_ = fix.position_m;
    velocity_ = fix.velocity_mps;
    previous_sample_ = sample;
    initialized_ = true;
}

unsigned MotionAverage::update(Fix& fix, uint64_t sample) {
    if (!fix.valid) {
        reset();
        return 0;
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(fix.position_m[axis]) || !std::isfinite(fix.velocity_mps[axis])) {
            fix = {};
            reset();
            return 0;
        }
    }
    constexpr auto interval = PositionSmoother::interval_samples;
    const Vec3 raw = fix.position_m;
    if (count_ && sample > previous_sample_ && sample - previous_sample_ == interval) {
        // Innovation is the previous code position transported to this epoch,
        // minus today's raw position. Subtract ECEF coordinates in double;
        // the difference between measured and Doppler motion fits in float.
        constexpr double half_dt = 0.5 * interval / sample_rate;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const double move = half_dt * (previous_velocity_[axis] + fix.velocity_mps[axis]);
            innovations_[next_][axis] = static_cast<float>(move - (raw[axis] - previous_position_[axis]));
        }
        if (++next_ == innovations_.size()) next_ = 0;
        count_ = std::min(count_ + 1, window);

        // Each innovation transports every older fix. The newest carries
        // weight count-1, descending to one for the oldest interval.
        std::array<float, 3> correction{};
        unsigned slot = next_;
        for (unsigned weight = count_-1; weight; --weight) {
            if (!slot) slot = innovations_.size();
            const auto& delta = innovations_[--slot];
            for (unsigned axis = 0; axis < 3; ++axis)
                correction[axis] += static_cast<float>(weight) * delta[axis];
        }
        for (unsigned axis = 0; axis < 3; ++axis)
            fix.position_m[axis] += correction[axis] / static_cast<float>(count_);
    } else {
        count_ = 1;
        next_ = 0;
    }
    previous_position_ = raw;
    previous_velocity_ = fix.velocity_mps;
    previous_sample_ = sample;
    return count_;
}

void VelocityFit::update(Fix& fix, uint64_t sample) {
    if (!fix.valid) {
        count_ = 0;
        return;
    }
    if (sample <= previous_sample_ || sample - previous_sample_ != PositionSmoother::interval_samples)
        count_ = 0;
    previous_sample_ = sample;
    const Vec3 raw = fix.velocity_mps;
    if (count_ < history_.size()) {
        history_[count_++] = raw;
        return;
    }
    // The line's endpoint weights on equally spaced epochs are [-1, 0, 1, 2, 3] / 5.
    for (unsigned i = 0; i < raw.size(); ++i)
        fix.velocity_mps[i] = (3 * raw[i] + 2 * history_[3][i] + history_[2][i] - history_[0][i]) * 0.2;
    std::move(history_.begin() + 1, history_.end(), history_.begin());
    history_.back() = raw;
}

bool solve_fix(const Observation* obs, std::size_t n, Fix& fix, double receiver_tow_s,
               [[maybe_unused]] const Ionosphere* ionosphere) {
    fix = {};
    if (!obs || n < 4 || n > 32) return false;
    const double rx = std::isnan(receiver_tow_s) ? week_time(obs[0].sv_tow_s + .075) : receiver_tow_s;
    if (!std::isfinite(rx)) return false;
    struct Measurement {
        Satellite sat;
        double range, rate;
    };
    std::array<Measurement,32> measurements;
    uint32_t seen = 0;
    int reference_week = -1;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& o = obs[i];
        if (o.prn > 32 || !o.ephemeris || !std::isfinite(o.doppler_hz) ||
            !(o.sv_tow_s >= 0 && o.sv_tow_s < week_seconds)) return false;
        if (o.prn != 0) {
            const uint32_t mask = 1u << (o.prn-1);
            if (seen & mask) return false;
            seen |= mask;
        }
        auto& value = measurements[i];
        double t = o.sv_tow_s;
        for (int j = 0; j < 2; ++j) {
            if (!propagate(*o.ephemeris,t,value.sat)) return false;
            t = week_time(o.sv_tow_s - value.sat.clock_s);
        }
        // Unwrap the receive epoch relative to this SV's decoded SF1 week.
        const double reference = o.ephemeris->sf1_tow;
        const double unwrapped = reference + week_diff(rx,reference);
        const int week = (o.ephemeris->week_mod1024 + int(std::floor(unwrapped/week_seconds)) + 1024) % 1024;
        if (reference_week != -1 && reference_week != week) return false;
        reference_week = week;
        value.range = speed_of_light * week_diff(rx,t);
        value.rate = -o.doppler_hz*(speed_of_light/l1_frequency) + speed_of_light*value.sat.clock_drift;
    }
    Fix trial{};
    bool converged = false;
#ifdef EPHEMERIS_GROUND_IONOSPHERE
    std::array<float,32> iono_m{};
    bool calculate_iono = false;
#endif
#if defined(EPHEMERIS_GROUND_TROPOSPHERE) || defined(EPHEMERIS_GROUND_IONOSPHERE)
    GroundAtmosphere atmosphere;
    bool checked_height = false;
#endif
    for (int iter = 0; iter < 12; ++iter) {
        LeastSquares ls;
        for (std::size_t i = 0; i < n; ++i) {
            const auto& m = measurements[i];
            const double travel = (m.range - trial.clock_bias_m)/speed_of_light;
            const Vec3 s = reception_axes(m.sat.position_m,travel);
            Vec3 line = subtract(trial.position_m,s);
            const double length = std::sqrt(dot(line,line));
            if (!(length > 1)) return false;
            for (double& x : line) x /= length;
            double model = length + trial.clock_bias_m;
#ifdef EPHEMERIS_GROUND_TROPOSPHERE
            if (atmosphere.zenith_m > 0) model += atmosphere.slant_m(line);
#endif
#ifdef EPHEMERIS_GROUND_IONOSPHERE
            if (calculate_iono)
                iono_m[i] = static_cast<float>(atmosphere.ionosphere_slant_m(line, week_time(rx - trial.clock_bias_m/speed_of_light), *ionosphere));
            model += iono_m[i];
#endif
            ls.add(line, m.range - model);
        }
        std::array<double,4> d{};
        if (!ls.solve(d,trial.pdop)) return false;
        for (int j = 0; j < 3; ++j) trial.position_m[j] += d[j];
        trial.clock_bias_m += d[3];
        const double step = std::max({std::abs(d[0]),std::abs(d[1]),std::abs(d[2]),std::abs(d[3])});
#ifdef EPHEMERIS_GROUND_IONOSPHERE
        calculate_iono = false;
#endif
#if defined(EPHEMERIS_GROUND_TROPOSPHERE) || defined(EPHEMERIS_GROUND_IONOSPHERE)
        if (!checked_height && step < 100) {
            checked_height = true;
            if (atmosphere.set_from_ecef(trial.position_m)) {
#ifdef EPHEMERIS_GROUND_IONOSPHERE
                calculate_iono = ionosphere != nullptr;
#endif
                continue;
            }
        }
#endif
        if (step < .001) {
            trial.range_rms_m = std::sqrt(ls.sumsq/n);
            converged = true;
            break;
        }
    }
    if (!converged || !(trial.range_rms_m < 100)) return false;
    // Differentiate the light-time equation. At orbital speed the transmit
    // epoch derivative (1 - range_rate/c) matters at the cm/s scale.
    LeastSquares ls;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = measurements[i];
        const double travel = (m.range-trial.clock_bias_m)/speed_of_light;
        const Vec3 s = reception_axes(m.sat.position_m,travel), v = reception_axes(m.sat.velocity_mps,travel);
        Vec3 line = subtract(trial.position_m,s);
        const double length = std::sqrt(dot(line,line));
        for (double& x : line) x /= length;
        const Vec3 dsdtravel = {earth_rate*s[1]-v[0], -earth_rate*s[0]-v[1], -v[2]};
        const double k = 1 + dot(line,dsdtravel)/speed_of_light;
        for (double& x : line) x /= k;
        ls.add(line, m.rate + dot(line,v));
    }
    double velocity_dop;
    std::array<double,4> v{};
    if (!ls.solve(v,velocity_dop)) return false;
    std::copy_n(v.begin(),3,trial.velocity_mps.begin());
    trial.clock_drift_mps = v[3];
    // Least-squares residual after applying the fitted velocity.
    double residual = ls.sumsq;
    for (int i=0; i<4; ++i) residual -= v[i]*ls.rhs[i];
    trial.rate_rms_mps = std::sqrt(std::max(0.,residual)/n);
    if (!(trial.rate_rms_mps < 20)) return false;
    const double corrected = week_time(rx) - trial.clock_bias_m/speed_of_light;
    trial.tow_s = week_time(corrected);
    trial.week_mod1024 = (reference_week + int(std::floor(corrected/week_seconds)) + 1024) % 1024;
    trial.satellites = static_cast<unsigned>(n);
    trial.valid = true;
    fix = trial;
    return true;
}
} // namespace eph::nav
