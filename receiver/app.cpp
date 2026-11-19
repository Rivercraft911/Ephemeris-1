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
constexpr uint64_t report_samples = sig::raw_per_ms * 200u;
uint32_t mask(unsigned prn) { return uint32_t{1} << (prn - 1); }
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
    occupied_.store(0, std::memory_order_release);
    status_.fix = {};
    status_.active_mask = status_.locked_mask = status_.valid_nav_mask = 0;
    have_clock_ = false;
    generation_.fetch_add(1, std::memory_order_release);
}

bool Receiver::seed_channel(const Detection& seed) {
    if (!seed.detected || seed.prn < 1 || seed.prn > 32 ||
        std::find(prns_.begin(), prns_.end(), seed.prn) != prns_.end()) return false;
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
    if (stage_ == Search::scanning) {
        if (acquisition_.step(1)) {
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
    if (stage_ == Search::collect) {
        if (!acquisition_.add_dwell(snapshot_)) stage_ = Search::select;
        else if (acquisition_.dwells() == sig::Acquisition::max_dwells) stage_ = Search::scanning;
        slot_.store(stage_ == Search::scanning ? Slot::idle : Slot::need_epoch,
                    std::memory_order_release);
        return true;
    }
    if (stage_ == Search::confirm) {
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
    if (__builtin_popcount(used) >= static_cast<int>(channels)) return false;
    unsigned prn = next_prn_;
    if (const auto preferred = priority_mask_ & ~used) {
        prn = 1u + static_cast<unsigned>(__builtin_ctz(preferred));
        priority_mask_ &= ~mask(prn);
    } else {
        while (used & mask(prn)) prn = prn % 32 + 1;
    }
    next_prn_ = prn % 32 + 1;
    float low = -70000, high = 70000;
    if (warm_mask_ & mask(prn)) {
        low = last_carrier_[prn - 1] - 2500;
        high = last_carrier_[prn - 1] + 2500;
        warm_mask_ &= ~mask(prn); // Fall back to full search if this narrow attempt misses.
    }
    if (!acquisition_.start(snapshot_, prn, low, high)) return false;
    stage_ = Search::collect;
    slot_.store(Slot::need_epoch, std::memory_order_release);
    return true;
}

void Receiver::navigate(uint64_t sample) {
    std::array<nav::Observation, channels> obs{};
    size_t n = 0;
    status_.valid_nav_mask = 0;
    for (unsigned i = 0; i < channels; ++i) {
        if (!prns_[i] || !results_[i].locked) continue;
        const auto* ephem = decoders_[i].ephemeris();
        if (ephem) cache_[prns_[i]-1] = *ephem;
        else if (decoders_[i].can_use_cached(cache_[prns_[i]-1])) ephem = &cache_[prns_[i]-1];
        else continue;
        double tx;
        if (ephem->iodc < 0 || !decoders_[i].transmit_time(static_cast<double>(sample), tx)) continue;
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
    nav::solve_fix(obs.data(), n, status_.fix, receiver_tow);
    status_.sample = sample;
    ++status_.sequence;
}

void Receiver::process(const uint8_t* packed, uint64_t first_sample, bool discontinuity) {
    const bool gap = discontinuity || (running_ && first_sample != next_sample_) || !packed;
    if (gap) {
        clear_channels();
        ++status_.gaps;
        status_.sample = first_sample;
        ++status_.sequence; // immediately invalidate even between normal reporting epochs
    }
    if (!running_ || gap) report_at_ = first_sample + report_samples;
    running_ = packed != nullptr;
    if (!running_) return;
    next_sample_ = first_sample + sig::raw_per_ms;
    sig::preprocess({first_sample, packed, sig::bytes_per_ms, gap}, live_);
    if (offer_ready_.load(std::memory_order_acquire)) {
        if (offer_generation_ == generation_.load(std::memory_order_acquire) &&
            first_sample >= offer_.first_sample && first_sample - offer_.first_sample < sig::raw_hz / 4)
            seed_channel(offer_);
        offer_ready_.store(false, std::memory_order_release);
    }
    status_.locked_mask = 0;
    for (unsigned i = 0; i < channels; ++i) {
        if (!prns_[i]) continue;
        auto& result = results_[i] = trackers_[i].update(live_);
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
        for (unsigned j = 0; j < result.count; ++j)
            decoders_[i].push_prompt(result.prompts[j].i, result.prompts[j].start_sample);
    }
    status_.active_mask = occupied_.load(std::memory_order_acquire);
    if (!gap && slot_.load(std::memory_order_acquire) == Slot::need_epoch) {
        snapshot_ = live_;
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
