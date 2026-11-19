// SPDX-License-Identifier: Apache-2.0
#include "signal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace eph::signal {
namespace {
constexpr double tau = 6.2831853071795864769;
constexpr int acq_length = samples_per_ms / 2; // exactly two samples per C/A chip
constexpr double bin_hz = acq_length * 1000.0 / fft_length;
constexpr std::uint32_t chip_unit = 1u << 22;
constexpr std::uint32_t code_period = code_length * chip_unit;
constexpr double turns32 = 4294967296.0;
// Equalized noncoherent means for 1..4 dwells. Each threshold is for a
// whole PRN search; acceptance also needs independent fresh code/carrier tests.
constexpr float acq_threshold[] = {0,13.0f,8.8f,6.9f,5.8f};
static_assert(fft_length >= 2 * acq_length - 1, "linear correlation needs zero padding");

// Each byte contributes x[0] - j*x[1]. A four-sample boxcar at +Fs/4
// is byte_pair[lo] - byte_pair[hi], without expanding four packed samples.
constexpr int level(unsigned bits) {
    constexpr int levels[] = {1, 3, -1, -3};
    return levels[bits & 3u];
}
constexpr auto make_pairs() {
    std::array<IQ8, 256> pairs{};
    for (unsigned byte = 0; byte < pairs.size(); ++byte) {
        pairs[byte] = {static_cast<std::int8_t>(level(byte >> 2) + level(byte >> 4)),
                       static_cast<std::int8_t>(level(byte) - level(byte >> 6))};
    }
    return pairs;
}
constexpr auto pairs = make_pairs();

// Compile-time quarter wave. Argument stays within [0, pi/2].
constexpr double quarter_sine(double x) {
    double term = x, sum = x;
    for (int n = 1; n <= 7; ++n) {
        term *= -x*x / ((2*n)*(2*n+1));
        sum += term;
    }
    return sum;
}
constexpr auto make_twiddles() {
    std::array<float, fft_length/4+1> out{};
    for (std::size_t n = 0; n < out.size(); ++n)
        out[n] = static_cast<float>(quarter_sine(tau*n/fft_length));
    return out;
}
constexpr auto quarter = make_twiddles();
constexpr float sine(unsigned k) {
    k &= fft_length-1;
    const unsigned x = k % (fft_length/4);
    switch (k / (fft_length/4)) {
        case 0: return quarter[x];
        case 1: return quarter[fft_length/4-x];
        case 2: return -quarter[x];
        default: return -quarter[fft_length/4-x];
    }
}
constexpr auto make_mixer() {
    std::array<IQ8, 256> out{};
    for (unsigned k = 0; k < out.size(); ++k) {
        const float c = sine(16*k+fft_length/4), s = sine(16*k);
        out[k] = {static_cast<std::int8_t>(127*c+(c>=0 ? 0.5f : -0.5f)),
                  static_cast<std::int8_t>(127*s+(s>=0 ? 0.5f : -0.5f))};
    }
    return out;
}
constexpr auto mixer = make_mixer();

using Spectrum = std::array<IQf, fft_length>;
void fft(Spectrum& a, bool inverse) {
    for (unsigned i = 1, j = 0; i < fft_length; ++i) {
        unsigned bit = fft_length/2;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (unsigned width = 2; width <= fft_length; width *= 2) {
        const unsigned half = width/2;
        for (unsigned k = 0; k < half; ++k) {
            const unsigned angle = k*(fft_length/width);
            const float c = sine(angle+fft_length/4);
            const float s = inverse ? sine(angle) : -sine(angle);
            for (unsigned p = k; p < fft_length; p += width) {
                const auto u = a[p], v = a[p+half];
                const float i = v.i*c - v.q*s, q = v.i*s + v.q*c;
                a[p] = {u.i+i, u.q+q};
                a[p+half] = {u.i-i, u.q-q};
            }
        }
    }
    // Unnormalized inverse. Peak / mean code-cell power cancels the FFT scale.
}
double wrap_chips(double phase) {
    phase = std::fmod(phase, double(code_length));
    return phase < 0 ? phase + code_length : phase;
}
double code_at(const Detection& seed, std::uint64_t sample) {
    const double dt = double(sample - seed.first_sample)/raw_hz;
    return wrap_chips(seed.code_phase_chips + dt*(chip_hz + seed.doppler_hz/1540.0));
}
std::uint32_t phase_step(double hz) {
    // Signed frequency represented modulo 2^32. Cast via signed integer for negatives.
    return static_cast<std::uint32_t>(std::llround(hz*turns32/(samples_per_ms*1000.0)));
}
bool valid_epoch(std::uint64_t first) {
    return first <= std::numeric_limits<std::uint64_t>::max() - raw_per_ms;
}
}  // namespace

IQ8 decode_nibble(unsigned nibble) {
    return {static_cast<std::int8_t>(level(nibble >> 2)), static_cast<std::int8_t>(level(nibble))};
}

bool preprocess(const PackedBlock& block, Epoch& epoch) {
    if (!block.bytes || block.size != bytes_per_ms || !valid_epoch(block.first_sample)) return false;
    epoch.first_sample = block.first_sample;
    epoch.discontinuity = block.discontinuity;
    for (int n = 0; n < samples_per_ms; ++n) {
        const IQ8 a = pairs[block.bytes[2*n]], b = pairs[block.bytes[2*n+1]];
        int i = a.i-b.i, q = a.q-b.q;
        switch (block.first_sample & 3u) {
            case 1: std::swap(i,q); q=-q; break;
            case 2: i=-i; q=-q; break;
            case 3: std::swap(i,q); i=-i; break;
        }
        epoch.samples[n] = {static_cast<std::int8_t>(i), static_cast<std::int8_t>(q)};
    }
    return true;
}

bool ca_code(unsigned prn, Code& out) {
    // IS-GPS-200N table 3-Ia, G2 phase selection. Stage 1 is the newest bit.
    constexpr int taps[][2] = {
        {2,6},{3,7},{4,8},{5,9},{1,9},{2,10},{1,8},{2,9},
        {3,10},{2,3},{3,4},{5,6},{6,7},{7,8},{8,9},{9,10},
        {1,4},{2,5},{3,6},{4,7},{5,8},{6,9},{1,3},{4,6},
        {5,7},{6,8},{7,9},{8,10},{1,6},{2,7},{3,8},{4,9}
    };
    if (prn < 1 || prn > 32) return false;
    unsigned g1=1023, g2=1023;
    const auto& t = taps[prn-1];
    for (auto& chip : out) {
        const unsigned bit = (g1>>9) ^ (g2>>(t[0]-1)) ^ (g2>>(t[1]-1));
        chip = (bit & 1u) ? -1 : 1;
        const unsigned fb1 = (g1>>2) ^ (g1>>9);
        const unsigned fb2 = (g2>>1) ^ (g2>>2) ^ (g2>>5) ^ (g2>>7) ^ (g2>>8) ^ (g2>>9);
        g1 = ((g1<<1) & 1023u) | (fb1 & 1u);
        g2 = ((g2<<1) & 1023u) | (fb2 & 1u);
    }
    return true;
}

bool Acquisition::start(const Epoch& epoch, unsigned prn, float lo_hz, float hi_hz) {
    best_ = {};
    dwells_ = current_dwell_ = 0;
    scanning_ = false;
    bin_ = 0;
    last_bin_ = -1;
    if (!valid_epoch(epoch.first_sample) || prn < 1 || prn > 32 ||
        !std::isfinite(lo_hz) || !std::isfinite(hi_hz) || lo_hz > hi_hz ||
        lo_hz < -100000.0f || hi_hz > 100000.0f) return false;
    if (prn != cached_prn_) {
        Code code;
        ca_code(prn, code);
        replica_.fill({});
        for (int n = 0; n < acq_length; ++n) replica_[n].i = code[n/2];
        fft(replica_, false);
        cached_prn_ = prn;
    }
    bin_ = int(std::floor(lo_hz/bin_hz));
    last_bin_ = int(std::ceil(hi_hz/bin_hz));
    best_.prn = prn;
    best_.first_sample = epoch.first_sample;
    return add_dwell(epoch);
}

bool Acquisition::add_dwell(const Epoch& epoch) {
    if (!best_.prn || scanning_ || dwells_ >= max_dwells || !valid_epoch(epoch.first_sample)) return false;
    if (dwells_ && (epoch.discontinuity || epoch.first_sample < epochs_[dwells_-1]+raw_per_ms ||
                    epoch.first_sample-best_.first_sample > 100*raw_per_ms)) return false;
    work_.fill({});
    float energy = 0;
    for (int n = 0; n < acq_length; ++n) {
        const auto a = epoch.samples[2*n], b = epoch.samples[2*n+1];
        const float i = float(a.i+b.i), q = float(a.q+b.q);
        work_[n] = {i,q};
        energy += i*i+q*q;
    }
    fft(work_, false);
    float peak = 1;
    for (const auto v : work_) peak = std::max({peak,std::abs(v.i),std::abs(v.q)});
    const float scale = 32766.0f / peak;
    for (int k=0;k<fft_length;++k) {
        auto quant = [scale](float v) { return static_cast<std::int16_t>(std::lround(v*scale)); };
        data_[dwells_][k] = {quant(work_[k].i),quant(work_[k].q)};
    }
    weights_[dwells_] = energy>0 ? 1.f/(energy*scale*scale) : 0;
    epochs_[dwells_++] = epoch.first_sample;
    return true;
}

bool Acquisition::step(unsigned max_transforms) {
    while (max_transforms-- && bin_ <= last_bin_) {
        scanning_ = true;
        if (!current_dwell_) power_.fill(0);
        const auto& spectral_data = data_[current_dwell_];
        // Wiping off an integer FFT-bin carrier is a rotation of the data spectrum.
        // Only one forward data FFT per dwell is required over the whole sweep.
        for (unsigned k = 0; k < fft_length; ++k) {
            const auto a = spectral_data[(k+static_cast<unsigned>(bin_)) & (fft_length-1)];
            const IQf b = replica_[k];
            work_[k] = {a.i*b.i + a.q*b.q, a.q*b.i - a.i*b.q};
        }
        fft(work_, true);
        // Align each dwell's code cells to the first sample of the search,
        // including both nonuniform snapshot spacing and carrier-aided code drift.
        const double dt = double(epochs_[current_dwell_]-epochs_[0])/raw_hz;
        const int shift = int(std::llround(2*dt*(chip_hz+bin_*bin_hz/1540.)))%acq_length;
        int aligned = shift;
        for (int k = 0; k < acq_length; ++k) {
            IQf r = work_[k];
            // Fold *linear* correlation at exactly 2046 samples, not 2048 or 4096.
            if (k) {
                const auto tail = work_[fft_length-acq_length+k];
                r.i += tail.i; r.q += tail.q;
            }
            power_[aligned] += (r.i*r.i+r.q*r.q)*weights_[current_dwell_];
            if (++aligned == acq_length) aligned=0;
        }
        if (++current_dwell_ < dwells_) continue;
        current_dwell_ = 0;
        float total=0, peak=0;
        int lag=0;
        for (int k=0;k<acq_length;++k) {
            total+=power_[k];
            if (power_[k]>peak) { peak=power_[k]; lag=k; }
        }
        const float metric = total > 0 ? peak*acq_length/total : 0;
        if (metric > best_.metric) {
            best_.metric = metric;
            best_.doppler_hz = float(bin_*bin_hz);
            best_.code_phase_chips = wrap_chips(-lag*0.5);
        }
        ++bin_;
    }
    const bool done = bin_ > last_bin_;
    best_.detected = done && dwells_ && best_.metric >= acq_threshold[dwells_];
    return done;
}

Detection confirm(const Detection& candidate, const Epoch& fresh) {
    Detection result = candidate;
    result.detected = false;
    result.metric = 0;
    if (!candidate.detected || fresh.discontinuity || !valid_epoch(candidate.first_sample) ||
        fresh.first_sample < candidate.first_sample+raw_per_ms ||
        !std::isfinite(candidate.doppler_hz) || std::abs(candidate.doppler_hz)>101000 ||
        !std::isfinite(candidate.code_phase_chips)) return result;
    Code code;
    if (!ca_code(candidate.prn, code)) return result;
    const double predicted = code_at(candidate, fresh.first_sample);
    const auto step = phase_step(candidate.doppler_hz);
    const double inc = (chip_hz+candidate.doppler_hz/1540.0)/(samples_per_ms*1000.0);
    // A half-chip acquisition grid and accumulated bin error permit a small neighborhood.
    for (int offset=-2; offset<=2; ++offset) {
        double cp = wrap_chips(predicted + 0.25*offset + inc*0.375);
        std::uint32_t phase = 0;
        std::int32_t si=0, sq=0, energy=0;
        for (const auto x : fresh.samples) {
            const auto lo = mixer[phase>>24];
            const int sign = code[static_cast<unsigned>(cp)];
            si += sign*(x.i*lo.i+x.q*lo.q);
            sq += sign*(x.q*lo.i-x.i*lo.q);
            energy += x.i*x.i+x.q*x.q;
            cp += inc;
            if (cp >= code_length) cp -= code_length;
            phase += step;
        }
        const float power = float(si)*si+float(sq)*sq;
        const float metric = energy ? power/(127.f*127.f*energy) : 0;
        if (metric > result.metric) {
            result.metric = metric;
            result.code_phase_chips = wrap_chips(predicted + 0.25*offset);
        }
    }
    result.first_sample = fresh.first_sample;
    result.detected = result.metric >= 9.0f;
    return result;
}

void Confirmation::start(const Detection& seed) {
    seed_=seed;
    best_=seed;
    best_.detected=false;
    best_.metric=0;
    dwells_=0;
    previous_sample_=seed.first_sample;
    power_.fill(0);
    if (!ca_code(seed.prn,code_)) seed_.detected=false;
}

bool Confirmation::add(const Epoch& fresh) {
    if (!seed_.detected || dwells_>=4 || fresh.discontinuity ||
        !valid_epoch(previous_sample_) || fresh.first_sample<previous_sample_+raw_per_ms ||
        !std::isfinite(seed_.doppler_hz) || std::abs(seed_.doppler_hz)>101000 ||
        !std::isfinite(seed_.code_phase_chips)) {
        best_.detected=false;
        return true;
    }
    previous_sample_ = fresh.first_sample;
    ++dwells_;
    // Fine carrier spacing and independent incoherent dwells preserve sensitivity
    // through unknown 50 bit/s LNAV inversions. Evaluate every delay in one pass.
    for (int f=0; f<3; ++f) {
        auto guess=seed_;
        guess.doppler_hz += float((f-1)*bin_hz/3);
        const auto step=phase_step(guess.doppler_hz);
        const auto phase=code_at(guess,fresh.first_sample);
        const double inc=(chip_hz+guess.doppler_hz/1540.0)/(samples_per_ms*1000.);
        const std::uint32_t cstep=static_cast<std::uint32_t>(std::llround(inc*chip_unit));
        std::uint32_t cp=static_cast<std::uint32_t>(wrap_chips(phase+inc*0.375-1)*chip_unit);
        std::uint32_t carrier=0;
        std::array<std::int32_t,9> si{},sq{};
        int energy=0;
        for (const auto x:fresh.samples) {
            const auto lo=mixer[carrier>>24];
            const int i=x.i*lo.i+x.q*lo.q, q=x.q*lo.i-x.i*lo.q;
            energy+=x.i*x.i+x.q*x.q;
            auto replica=cp;
            for (int lag=0;lag<9;++lag) {
                const int c=code_[replica>>22];
                si[lag]+=i*c; sq[lag]+=q*c;
                replica+=chip_unit/4;
                if (replica>=code_period) replica-=code_period;
            }
            cp+=cstep;
            if (cp>=code_period) cp-=code_period;
            carrier+=step;
        }
        const float weight=energy ? 1.f/(127.f*127.f*energy) : 0;
        for (int lag=0;lag<9;++lag) {
            auto& score=power_[9*f+lag];
            score+=weight*(float(si[lag])*si[lag]+float(sq[lag])*sq[lag]);
            if (dwells_==4 && score/4>best_.metric) {
                best_=guess;
                best_.first_sample=fresh.first_sample;
                best_.code_phase_chips=wrap_chips(phase+(lag-4)*0.25);
                best_.metric=score/4;
            }
        }
    }
    best_.detected=dwells_==4 && best_.metric>=5.25f;
    return dwells_==4;
}

bool Tracker::start(const Detection& detection) {
    active_ = false;
    if (!detection.detected || !valid_epoch(detection.first_sample) ||
        !std::isfinite(detection.code_phase_chips) || !std::isfinite(detection.doppler_hz) ||
        std::abs(detection.doppler_hz)>101000 || !ca_code(detection.prn, code_)) return false;
    seed_ = detection;
    initialized_ = full_period_ = have_previous_ = false;
    weak_ = good_ = 0;
    epoch_number_ = 0;
    code_residual_hz_ = step_remainder_ = 0;
    doppler_ = detection.doppler_hz;
    carrier_step_ = phase_step(doppler_);
    carrier_phase_ = 0;
    ei_=eq_=pi_=pq_=li_=lq_=energy_=0;
    integrated_ = averaging_count_ = good_phases_ = mean_count_ = smooth_count_ = 0;
    local_turns_ = time_sum_ = phase_sum_ = 0;
    fit_valid_ = false;
    active_ = true;
    return true;
}

void Tracker::initialize_phase(std::uint64_t sample) {
    const double rate = chip_hz + doppler_/1540.0;
    const double phase = wrap_chips(code_at(seed_, sample) + 1.5*rate/raw_hz);
    code_phase_ = static_cast<std::uint32_t>(phase*chip_unit);
    code_step_ = static_cast<std::uint32_t>(std::llround(rate*chip_unit/(samples_per_ms*1000.0)));
    period_start_ = double(sample) + 1.5 - phase*raw_hz/rate;
    initialized_ = true;
}

double Tracker::measured_doppler(double sample) const {
    if (!fit_valid_) return doppler_;
    return fit_hz_ + fit_hz_per_s_*(sample-fit_sample_)/raw_hz;
}

void Tracker::observe_phase(double sample, double turns, bool good) {
    // A tracking-loop tuning word contains 1 ms discriminator noise. Recover the
    // measured carrier phase in a continuous frame, average three 160-epoch
    // windows, then differentiate their quadratic interpolant at the report time.
    // The acceleration term avoids the velocity lag of a trailing moving average.
    if (good) {
        time_sum_ += sample;
        phase_sum_ += turns;
        ++good_phases_;
    }
    if (++averaging_count_ < 160) return;
    if (good_phases_ >= 50) {
        means_[0] = means_[1];
        means_[1] = means_[2];
        means_[2] = {time_sum_/good_phases_,phase_sum_/good_phases_};
        mean_count_ = std::min(3u,mean_count_+1);
        if (mean_count_ == 3) {
            const auto& a=means_[0]; const auto& b=means_[1]; const auto& c=means_[2];
            const double v0 = (b.turns-a.turns)*raw_hz/(b.sample-a.sample);
            const double v1 = (c.turns-b.turns)*raw_hz/(c.sample-b.sample);
            fit_hz_per_s_ = 2*(v1-v0)*raw_hz/(c.sample-a.sample);
            fit_sample_ = 0.5*(c.sample+b.sample);
            fit_hz_ = v1;
            fit_valid_ = true;
        }
    } else {
        fit_valid_ = false;
        mean_count_ = 0;
    }
    phase_sum_ = time_sum_ = 0;
    averaging_count_ = good_phases_ = 0;
}

void Tracker::finish_period(double boundary, TrackingResult& result) {
    // Integrated NCO phase at the next sample center, BEFORE the loop
    // correction. The signed step handles carrier wipeoff on either Doppler side.
    const double turns_per_sample = (carrier_step_ < 0x80000000u ? double(carrier_step_) :
                                     double(carrier_step_)-turns32)/turns32;
    const double total_turns = integrated_*turns_per_sample;
    const double midpoint_turns = local_turns_ + 0.5*(integrated_-1)*turns_per_sample;
    local_turns_ += total_turns;
    double next_smooth_boundary = boundary;
    if (full_period_) {
        const float i = pi_/127.f, q = pq_/127.f;
        const float power = i*i+q*q;
        const float snr_metric = energy_ ? power/energy_ : 0;
        result.prompt_i = i; result.prompt_q = q; result.symbol = i >= 0 ? 1 : -1;
        // Use the centers of the samples that actually contributed. The code
        // crossing is fractional, particularly important at large LEO Doppler.
        const double center_sample = boundary + 4.0*code_phase_/code_step_ - 2.0*(integrated_+1);
        const double polarity = i >= 0 ? 1.0 : -1.0;
        const double residual_turns = std::atan2(q*polarity,i*polarity)/tau;
        observe_phase(center_sample,midpoint_turns+residual_turns,snr_metric>=4.0f);
        // A short carrier-smoothed code interval reduces DLL jitter for
        // pseudoranges. The DLL still controls the code actually correlated.
        smooth_count_ = std::min(smooth_count_+1, 4000u);
        const double predicted = smooth_boundary_ + raw_hz*code_length/
                                 (chip_hz+measured_doppler(center_sample)/1540.);
        const double alpha = 1.0/smooth_count_;
        next_smooth_boundary = predicted+alpha*(boundary-predicted);
        result.prompts[result.count++] = {epoch_number_, smooth_boundary_, next_smooth_boundary,
                                           i, q, result.symbol};
        if (snr_metric >= 4.0f) {
            weak_ = 0;
            good_ = std::min(good_+1, 4u);
            const float early = std::hypot(float(ei_),float(eq_));
            const float late = std::hypot(float(li_),float(lq_));
            const float dll = early+late > 0 ? (early-late)/(early+late) : 0;
            // A proportional discriminator adjusts code NCO rate continuously.
            // Carrier aiding handles the line-of-sight range rate on both signals.
            code_residual_hz_ = 40.0*dll;
            const float error = static_cast<float>(residual_turns*tau);
            // A wide pull-in loop handles the initial ±250 Hz FFT-bin error.
            // Once settled, narrow it to keep noise out of carrier phase.
            const bool settled = good_ == 4 && epoch_number_ >= 48;
            const float correction = (settled ? 0.14f : 0.40f)*error;
            if (have_previous_) {
                const float cross = q*previous_.i - i*previous_.q;
                const float dot = i*previous_.i + q*previous_.q;
                const float p = dot >= 0 ? 1.f : -1.f;
                const float delta = std::atan2(cross*p, dot*p);
                const float dt = float((boundary-period_start_)/raw_hz);
                doppler_ += (settled ? 0.018f : 0.30f)*delta/(float(tau)*dt);
            }
            doppler_ += 10.0f*error/float(tau);
            carrier_phase_ += static_cast<std::uint32_t>(std::llround(correction*turns32/tau));
            local_turns_ += correction/tau;
            carrier_step_ = phase_step(doppler_);
            // Compare the next prompt to this one in the *corrected* NCO frame.
            const float c = std::cos(correction), s = std::sin(correction);
            previous_ = {i*c+q*s, q*c-i*s};
            have_previous_ = true;
        } else {
            if (good_ < 4) good_ = 0;
            have_previous_ = false;
            if (++weak_ >= 15) active_ = false;
        }
    }
    // Error feedback avoids accumulating the integer tuning word's rounding
    // error at sub-chip timing resolution across many 1 ms integrations.
    const double desired = (chip_hz + doppler_/1540.0 + code_residual_hz_)*
                           chip_unit/(samples_per_ms*1000.0) + step_remainder_;
    code_step_ = static_cast<std::uint32_t>(std::llround(desired));
    step_remainder_ = desired-code_step_;
    smooth_boundary_ = next_smooth_boundary;
    integrated_ = 0;
    period_start_ = boundary;
    full_period_ = true;
    ++epoch_number_;
    ei_=eq_=pi_=pq_=li_=lq_=energy_=0;
}

TrackingResult Tracker::update(const Epoch& epoch) {
    TrackingResult out{};
    out.prn = seed_.prn;
    out.first_sample = epoch.first_sample;
    if (!active_ || epoch.discontinuity || !valid_epoch(epoch.first_sample) ||
        epoch.first_sample < seed_.first_sample || (initialized_ && epoch.first_sample != next_sample_)) {
        active_ = false;
        return out;
    }
    if (!initialized_) initialize_phase(epoch.first_sample);
    next_sample_ = epoch.first_sample + raw_per_ms;
    out.code_phase_chips = wrap_chips((code_phase_ - 0.375*code_step_)/chip_unit);
    for (int n = 0; n < samples_per_ms; ++n) {
        const auto x = epoch.samples[n];
        const auto lo = mixer[carrier_phase_>>24];
        const int i = x.i*lo.i + x.q*lo.q;
        const int q = x.q*lo.i - x.i*lo.q;
        // Average each local early/late chip replica over the same four ADC
        // samples as the input. A nearest-chip replica at 4 samples/chip has
        // a discontinuous discriminator and can bias pseudorange as code drifts.
        // E and L are exactly one chip apart, so they share one fractional weight.
        std::uint32_t edge = code_phase_ + chip_unit/2 + chip_unit/8;
        if (edge >= code_period) edge -= code_period;
        const unsigned k=edge>>22, prev=k ? k-1 : code_length-1;
        const auto fraction=edge & (chip_unit-1);
        int e=32*code_[k], l=32*code_[prev];
        if (fraction < chip_unit/4) {
            const int weight=static_cast<int>((fraction+(1u<<14))>>15);
            const int prior=code_[prev ? prev-1 : code_length-1];
            e=32*code_[prev] + weight*(code_[k]-code_[prev]);
            l=32*prior + weight*(code_[prev]-prior);
        }
        const int p=code_[code_phase_>>22];
        ei_+=e*i; eq_+=e*q; pi_+=p*i; pq_+=p*q; li_+=l*i; lq_+=l*q;
        energy_+=x.i*x.i+x.q*x.q;
        ++integrated_;
        carrier_phase_ += carrier_step_;
        code_phase_ += code_step_;
        if (code_phase_ >= code_period) {
            code_phase_ -= code_period;
            const double next_center = double(epoch.first_sample) + 4.0*(n+1) + 1.5;
            finish_period(next_center - 4.0*code_phase_/code_step_, out);
            if (!active_) break;
        }
    }
    out.active=active_; out.locked=active_ && good_>=4;
    out.code_epochs=epoch_number_;
    out.doppler_hz=static_cast<float>(measured_doppler(double(next_sample_)));
    return out;
}
} // namespace eph::signal
