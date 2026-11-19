// SPDX-License-Identifier: Apache-2.0
#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <limits>

namespace ephemeris {
namespace sig = eph::signal;
namespace nav = eph::nav;
namespace {
constexpr uint64_t report_samples = nav::PositionSmoother::interval_samples;
uint32_t mask(unsigned prn) { return uint32_t{1} << (prn - 1); }

struct ColdSearch {
    unsigned prn;
    float span_hz;
    bool survey;
};

constexpr ColdSearch cold_search(unsigned index) {
#ifdef EPHEMERIS_COMPACT_SURVEY
    // Survey all 32 codes in the inner band, then all 32 across the full band.
    // Thereafter, give each four-dwell search four quick revisits in between.
    if (index < 64)
        return {1u + index % 32, index < 32 ? 45000.f : 70000.f, true};
    const unsigned slot = index - 64;
    const unsigned group = slot / 5, offset = slot % 5;
    if (!offset) return {1u + group % 32, 70000.f, false};
    const unsigned quick = 4 * group + offset - 1;
    return {1u + quick % 32, quick & 32u ? 70000.f : 45000.f, true};
#else
    ColdSearch search{1u + index % 32, 70000.f, false};
#ifdef EPHEMERIS_GROUND_SEARCH
    if (!(index & 32u)) search.span_hz = 12000.f;
#endif
#ifdef EPHEMERIS_FLIGHT_SURVEY
#ifdef EPHEMERIS_GROUND_SEARCH
    search.survey = (index & 96u) == 32u;
#else
    search.survey = !(index & 32u);
#endif
#endif
    return search;
#endif
}
}

namespace {
uint32_t crc32(const uint8_t* bytes, size_t count) {
    uint32_t crc = ~uint32_t{0};
    for (size_t i=0; i<count; ++i) {
        crc ^= bytes[i];
        for (unsigned b=0; b<8; ++b) crc = (crc>>1) ^ ((0u-(crc&1u)) & 0xedb88320u);
    }
    return ~crc;
}
}

void Receiver::save_warm(WarmState& out) const {
    static_assert(std::is_trivially_copyable_v<nav::Ephemeris>);
    static_assert(sizeof(float) == 4);
    std::memset(out.bytes,0,sizeof(out.bytes));
    std::memcpy(out.bytes,"EPW1",4);
    for (unsigned i=0; i<channels; ++i) {
        if (!prns_[i] || !results_[i].locked) continue;
        const uint32_t prn = prns_[i];
        auto dest = out.bytes+4+i*WarmState::record_size;
        std::memcpy(dest,&prn,4);
        std::memcpy(dest+4,&results_[i].doppler_hz,4);
        std::memcpy(dest+8,&cache_[prn-1],sizeof(nav::Ephemeris));
    }
    const auto sum = crc32(out.bytes,sizeof(out.bytes)-4);
    std::memcpy(out.bytes+sizeof(out.bytes)-4,&sum,4);
}

bool Receiver::restore_warm(const WarmState& saved) {
    uint32_t checksum;
    std::memcpy(&checksum,saved.bytes+sizeof(saved.bytes)-4,4);
    if (running_ || std::memcmp(saved.bytes,"EPW1",4) ||
        crc32(saved.bytes,sizeof(saved.bytes)-4) != checksum) return false;
    uint32_t used = 0;
    for (unsigned i=0; i<channels; ++i) {
        const auto record = saved.bytes+4+i*WarmState::record_size;
        uint32_t prn;
        float doppler;
        std::memcpy(&prn,record,4);
        std::memcpy(&doppler,record+4,4);
        if (!prn) continue;
        if (prn>32 || (used & mask(prn)) || !std::isfinite(doppler) ||
            std::abs(doppler)>100000) return false;
        used |= mask(prn);
    }
    for (unsigned i=0; i<channels; ++i) {
        const auto record = saved.bytes+4+i*WarmState::record_size;
        uint32_t prn;
        std::memcpy(&prn,record,4);
        if (!prn) continue;
        std::memcpy(&last_carrier_[prn-1],record+4,4);
        std::memcpy(&cache_[prn-1],record+8,sizeof(nav::Ephemeris));
    }
    warm_mask_ = priority_mask_ = used;
    return true;
}

void Receiver::clear_channels() {
    for (unsigned i = 0; i < channels; ++i) {
        trackers_[i].reset();
        decoders_[i].reset();
        results_[i] = {};
    }
    prns_.fill(0);
#ifdef EPHEMERIS_FLIGHT_HEALTH
    deferred_.store(0, std::memory_order_release);
#endif
    occupied_.store(0, std::memory_order_release);
    status_.fix = {};
#ifdef EPHEMERIS_MOTION_AVERAGE
    motion_average_.reset();
    status_.raw_position_m = {};
    status_.motion_samples = 0;
#endif
#ifdef EPHEMERIS_DOPPLER_POSITION
    position_smoother_ = {};
#endif
#ifdef EPHEMERIS_VELOCITY_FIT
    velocity_fit_ = {};
#endif
    status_.active_mask = status_.locked_mask = status_.valid_nav_mask = 0;
    have_clock_ = false;
    generation_.fetch_add(1, std::memory_order_release);
}

bool Receiver::seed_channel(const Detection& seed) {
    if (!seed.detected || seed.prn < 1 || seed.prn > 32 ||
        std::find(prns_.begin(), prns_.end(), seed.prn) != prns_.end()) return false;
#ifdef EPHEMERIS_FLIGHT_HEALTH
    if (deferred_.load(std::memory_order_relaxed) & mask(seed.prn)) return false;
#endif
    for (unsigned i = 0; i < channels; ++i) {
        if (prns_[i] || !trackers_[i].start(seed)) continue;
        prns_[i] = seed.prn;
        decoders_[i].reset();
        occupied_.fetch_or(mask(seed.prn), std::memory_order_release);
        return true;
    }
    return false;
}

bool Receiver::work_acquisition() {
    const auto generation = generation_.load(std::memory_order_acquire);
    if (worker_generation_ != generation) {
        worker_generation_ = generation;
        stage_ = Search::select;
        slot_.store(Slot::need_epoch, std::memory_order_release);
    }
    if (offer_ready_.load(std::memory_order_acquire)) return false;
    if (stage_ == Search::scanning || stage_ == Search::survey) {
        if (stage_ == Search::survey ? acquisition_.step_survey(1) : acquisition_.step(1)) {
            candidate_ = acquisition_.best();
            stage_ = candidate_.detected ? Search::confirm : Search::select;
            if (candidate_.detected) confirmation_.start(candidate_);
            slot_.store(Slot::need_epoch, std::memory_order_release);
        }
        return true;
    }
    if (slot_.load(std::memory_order_acquire) != Slot::ready) return false;
    if (snapshot_generation_ != generation) {
        slot_.store(Slot::need_epoch, std::memory_order_release);
        return false;
    }
#if defined(EPHEMERIS_BITPLANE_TRACKING) || defined(EPHEMERIS_COARSE_TRACKING)
    // Acquisition owns this slot until it asks for another epoch. Each input
    // byte pair becomes one IQ8, so it can downconvert in the same storage.
    const auto* raw = reinterpret_cast<const uint8_t*>(snapshot_.samples.data());
    sig::preprocess({snapshot_.first_sample, raw, sig::bytes_per_ms, false}, snapshot_);
#endif
    if (stage_ == Search::collect) {
        if (!acquisition_.add_dwell(snapshot_)) stage_ = Search::select;
        else if (acquisition_.dwells() == sig::Acquisition::max_dwells) stage_ = Search::scanning;
        slot_.store(stage_ == Search::scanning ? Slot::idle : Slot::need_epoch,
                    std::memory_order_release);
        return true;
    }
    if (stage_ == Search::confirm) {
        if (may_refresh_) {
            may_refresh_ = false;
            if (snapshot_.first_sample >= candidate_.first_sample &&
                snapshot_.first_sample - candidate_.first_sample >= uint64_t(sig::raw_hz)) {
                // A coarse frequency bin can be 250 Hz off at the first dwell.
                // Allow either sign of Doppler slew (150 Hz/s design envelope)
                // and search every code phase again on four current epochs.
                const double age_s = double(snapshot_.first_sample - candidate_.first_sample) / sig::raw_hz;
                const float radius = float(std::min(5000., 250. + 150.*age_s));
                const float center = candidate_.doppler_hz;
                const bool started = acquisition_.start(snapshot_, candidate_.prn,
                    std::max(-100000.f, center-radius), std::min(100000.f, center+radius));
                stage_ = started ? Search::collect : Search::select;
                slot_.store(Slot::need_epoch, std::memory_order_release);
                return true;
            }
        }
        if (!confirmation_.add(snapshot_)) {
            slot_.store(Slot::need_epoch, std::memory_order_release);
            return true;
        }
        const auto hit = confirmation_.best();
        if (hit.detected) {
            offer_ = hit;
            offer_generation_ = generation;
            last_carrier_[hit.prn - 1] = hit.doppler_hz;
            warm_mask_ |= mask(hit.prn);
            offer_ready_.store(true, std::memory_order_release);
        }
        stage_ = Search::select;
        slot_.store(Slot::need_epoch, std::memory_order_release);
        return true;
    }
    const auto used = occupied_.load(std::memory_order_acquire);
    auto skip = used;
#ifdef EPHEMERIS_FLIGHT_HEALTH
    skip |= deferred_.load(std::memory_order_acquire);
#endif
    if (skip == UINT32_MAX || __builtin_popcount(used) >= static_cast<int>(channels)) {
        slot_.store(Slot::need_epoch, std::memory_order_release);
        return false;
    }
    unsigned prn;
    bool survey = false;
    float span = 70000;
    if (const auto preferred = priority_mask_ & ~skip) {
        prn = 1u + static_cast<unsigned>(__builtin_ctz(preferred));
        priority_mask_ &= ~mask(prn);
    } else {
        do {
            const auto search = cold_search(search_index_++);
            prn = search.prn;
            survey = search.survey;
            span = search.span_hz;
        } while (skip & mask(prn));
    }
    float low = -span, high = span;
    if (warm_mask_ & mask(prn)) {
        survey = false;
        low = last_carrier_[prn - 1] - 2500;
        high = last_carrier_[prn - 1] + 2500;
        warm_mask_ &= ~mask(prn); // Rejoin normal cold search after one cached attempt.
    }
    const bool started = survey ? acquisition_.start_survey(snapshot_, prn, low, high)
                                : acquisition_.start(snapshot_, prn, low, high);
    if (!started) {
        slot_.store(Slot::need_epoch, std::memory_order_release);
        return false;
    }
    may_refresh_ = true;
    stage_ = survey ? Search::survey : Search::collect;
    slot_.store(survey ? Slot::idle : Slot::need_epoch, std::memory_order_release);
    return true;
}

void Receiver::navigate(uint64_t sample) {
#ifdef EPHEMERIS_FLIGHT_HEALTH
    // The low 16 bits wrap every 13,107 s. Every report is visited, including
    // reports without a fix, and a capture discontinuity clears the mask.
    for (auto bits = deferred_.load(std::memory_order_relaxed); bits; bits &= bits-1) {
        const unsigned n = static_cast<unsigned>(__builtin_ctz(bits));
        if (static_cast<uint16_t>(status_.sequence) == retry_report_[n])
            deferred_.fetch_and(~(uint32_t{1} << n), std::memory_order_release);
    }
#endif
    std::array<nav::Observation, channels> obs{};
    size_t n = 0;
    const nav::Ionosphere* ionosphere = nullptr;
#ifdef EPHEMERIS_GROUND_IONOSPHERE
    static constexpr nav::Ionosphere cold_model{};
    ionosphere = &cold_model;
#endif
    status_.valid_nav_mask = 0;
    for (unsigned i = 0; i < channels; ++i) {
        if (!prns_[i] || !results_[i].locked) continue;
#ifdef EPHEMERIS_GROUND_IONOSPHERE
        const auto& page = decoders_[i].ionosphere();
        if (page.fresh(double(sample)) && page.received_sample > ionosphere->received_sample)
            ionosphere = &page;
#endif
        const auto* ephem = decoders_[i].ephemeris();
        if (ephem) cache_[prns_[i]-1] = *ephem;
        else if (decoders_[i].can_use_cached(cache_[prns_[i]-1])) ephem = &cache_[prns_[i]-1];
        else continue;
        double tx;
        if (!decoders_[i].transmit_time(static_cast<double>(sample), tx) || !nav::usable_at(*ephem, tx)) continue;
        obs[n++] = {ephem, tx, results_[i].doppler_hz, prns_[i]};
        status_.valid_nav_mask |= mask(prns_[i]);
    }
    if (n >= 4 && !have_clock_) {
        clock_sample_ = sample;
        clock_tow_ = obs[0].sv_tow_s + 0.075;
        have_clock_ = true;
    }
    const double receiver_tow = have_clock_ ?
        clock_tow_ + static_cast<double>(sample - clock_sample_) / sig::raw_hz :
        std::numeric_limits<double>::quiet_NaN();
    nav::solve_fix(obs.data(), n, status_.fix, receiver_tow, ionosphere);
#ifdef EPHEMERIS_DOPPLER_POSITION
    position_smoother_.update(status_.fix, sample);
#endif
#ifdef EPHEMERIS_VELOCITY_FIT
    velocity_fit_.update(status_.fix, sample);
#endif
#ifdef EPHEMERIS_MOTION_AVERAGE
    const auto raw_position = status_.fix.position_m;
    status_.motion_samples = motion_average_.update(status_.fix, sample);
    status_.raw_position_m = status_.fix.valid ? raw_position : nav::Vec3{};
#endif
    status_.sample = sample;
    ++status_.sequence;
}

void Receiver::set_tracking_worker(WorkerHook notify, WorkerHook wait, void* context) {
    tracking_notify_ = notify;
    tracking_wait_ = wait;
    tracking_context_ = context;
}

void Receiver::track_alternating(unsigned first) {
    for (unsigned i = first; i < channels; i += 2)
        if (prns_[i]) results_[i] = trackers_[i].update(live_);
}

bool Receiver::work_tracking() {
    if (!tracking_pending_.load(std::memory_order_acquire)) return false;
    track_alternating(1);
    tracking_pending_.store(false, std::memory_order_release);
    return true;
}

void Receiver::process(const uint8_t* packed, uint64_t first_sample, bool discontinuity) {
    const bool gap = discontinuity || (running_ && first_sample != next_sample_) || !packed;
    if (gap) {
        clear_channels();
        ++status_.gaps;
        status_.sample = first_sample;
        ++status_.sequence; // immediately invalidate even between normal reporting epochs
    }
    if (!running_ || gap) {
        report_at_ = first_sample + report_samples;
        for (auto& tracker : trackers_) tracker.set_report_origin(first_sample);
    }
    running_ = packed != nullptr;
    if (!running_) return;
    next_sample_ = first_sample + sig::raw_per_ms;
    const sig::PackedBlock block{first_sample, packed, sig::bytes_per_ms, gap};
    if (offer_ready_.load(std::memory_order_acquire)) {
        if (offer_generation_ == generation_.load(std::memory_order_acquire) &&
            first_sample >= offer_.first_sample && first_sample - offer_.first_sample < sig::raw_hz / 4)
            seed_channel(offer_);
        offer_ready_.store(false, std::memory_order_release);
    }
#ifdef EPHEMERIS_COARSE_TRACKING
    if (occupied_.load(std::memory_order_relaxed)) sig::coarse::preprocess(block,live_);
#elif defined(EPHEMERIS_BITPLANE_TRACKING)
    if (occupied_.load(std::memory_order_relaxed)) sig::bitplane::from_packed(block,live_);
#else
    sig::preprocess(block,live_);
#endif
    bool parallel = false;
    if (tracking_notify_) {
        for (unsigned i = 1; i < channels; i += 2) parallel |= prns_[i] != 0;
    }
    if (parallel) {
        // The owner freezes live_ and the slot assignments until both halves
        // finish. Workers mutate disjoint Tracker and TrackingResult objects.
        tracking_pending_.store(true, std::memory_order_release);
        tracking_notify_(tracking_context_);
        track_alternating(0);
        while (tracking_pending_.load(std::memory_order_acquire)) {
            if (tracking_wait_) tracking_wait_(tracking_context_);
        }
    }
    status_.locked_mask = 0;
    for (unsigned i = 0; i < channels; ++i) {
        if (!prns_[i]) continue;
        auto& result = results_[i];
        if (!parallel) result = trackers_[i].update(live_);
        if (result.locked) {
            for (unsigned j = 0; j < result.count; ++j)
                decoders_[i].push_prompt(result.prompts[j].i, result.prompts[j].start_sample);
#ifdef EPHEMERIS_FLIGHT_HEALTH
            if (decoders_[i].broadcast_unhealthy()) {
                const unsigned prn = prns_[i];
                // Retry after two 30 s LNAV frames at the 5 Hz reporting rate.
                retry_report_[prn-1] = static_cast<uint16_t>(status_.sequence + 300);
                deferred_.fetch_or(mask(prn), std::memory_order_release);
                trackers_[i].reset();
                result = {};
            }
#endif
        }
        if (!result.active) {
            decoders_[i].reset();
            occupied_.fetch_and(~mask(prns_[i]), std::memory_order_release);
            prns_[i] = 0;
            continue;
        }
        if (!result.locked) {
            decoders_[i].reset();
            continue;
        }
        status_.locked_mask |= mask(prns_[i]);
    }
    status_.active_mask = occupied_.load(std::memory_order_acquire);
    if (!gap && __builtin_popcount(status_.active_mask) < static_cast<int>(channels) &&
        slot_.load(std::memory_order_acquire) == Slot::need_epoch) {
#if defined(EPHEMERIS_BITPLANE_TRACKING) || defined(EPHEMERIS_COARSE_TRACKING)
        static_assert(sizeof(snapshot_.samples) == sig::bytes_per_ms);
        snapshot_.first_sample = first_sample;
        std::memcpy(snapshot_.samples.data(), packed, sig::bytes_per_ms);
#else
        snapshot_ = live_;
#endif
        snapshot_generation_ = generation_.load(std::memory_order_relaxed);
        slot_.store(Slot::ready, std::memory_order_release);
    }
    if (next_sample_ >= report_at_) {
        navigate(next_sample_);
        report_at_ += report_samples;
    }
}

int format_status(char* dst, size_t cap, const Status& status) {
    const auto& f = status.fix;
    return std::snprintf(dst, cap,
        "%u,%llu,%u,%u,%u,%u,%u,%d,%.9f,%.3f,%.3f,%.3f,%.5f,%.5f,%.5f,%.3f,%.5f,%u,%.3f",
        unsigned(status.sequence), static_cast<unsigned long long>(status.sample), unsigned(status.gaps),
        unsigned(status.active_mask), unsigned(status.locked_mask), unsigned(status.valid_nav_mask),
        unsigned(f.valid), f.week_mod1024, f.tow_s, f.position_m[0], f.position_m[1], f.position_m[2],
        f.velocity_mps[0], f.velocity_mps[1], f.velocity_mps[2], f.clock_bias_m, f.clock_drift_mps,
        f.satellites, f.pdop);
}
} // namespace ephemeris
