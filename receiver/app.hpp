// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "navigation.hpp"
#include "signal.hpp"
#ifdef EPHEMERIS_BITPLANE_TRACKING
#include "bitplane.hpp"
#endif

#include <array>
#include <atomic>
#include <cstdint>

namespace ephemeris {

// Same-image watchdog retention only. No wall time, decoded frame alignment or fix is retained.
struct WarmState {
    static constexpr size_t record_size = 2 * sizeof(uint32_t) + sizeof(eph::nav::Ephemeris);
    uint8_t bytes[8 + 8 * record_size];
};

struct Status {
    uint32_t sequence{}, gaps{}, active_mask{}, locked_mask{}, valid_nav_mask{};
    uint64_t sample{}; // common measurement epoch, in raw ADC samples
    eph::nav::Fix fix{};
};
inline constexpr char status_header[] =
    "sequence,sample,gaps,active_mask,locked_mask,nav_mask,valid,week_mod1024,tow_s,"
    "x_m,y_m,z_m,vx_mps,vy_mps,vz_mps,bias_m,drift_mps,sv,pdop";
int format_status(char* dst, size_t capacity, const Status& status);

class Receiver {
public:
    static constexpr unsigned channels = 8;
    // One acquisition worker, one capture/processing thread. No shared mutable DSP buffers.
    void process(const uint8_t* packed, uint64_t first_sample, bool discontinuity);
    bool work_acquisition(); // perform one bounded search step, or return false if waiting
    void save_warm(WarmState& out) const; // capture owner only
    bool restore_warm(const WarmState& saved); // boot only, before the worker is started
    const Status& status() const { return status_; }
    // Explicitly aided lab entry point. Normal boots call process() with an empty channel bank.
    bool seed_channel(const eph::signal::Detection& seed);
    const eph::signal::TrackingResult& channel(unsigned index) const { return results_[index]; }
    bool channel_tx_tow(unsigned index, double sample, double& tow_s) const {
        return index < channels && decoders_[index].transmit_time(sample, tow_s);
    }
    unsigned subframes(unsigned index) const { return decoders_[index].good_subframes(); }

private:
    using Detection = eph::signal::Detection;
    enum class Slot : unsigned { need_epoch, ready, idle };
    enum class Search : unsigned { select, collect, scanning, confirm };
    std::array<eph::signal::Tracker, channels> trackers_{};
    std::array<eph::signal::TrackingResult, channels> results_{};
    std::array<eph::nav::LnavDecoder, channels> decoders_{};
    std::array<eph::nav::Ephemeris, 32> cache_{};
    std::array<unsigned, channels> prns_{};
    eph::signal::Epoch snapshot_{};
#if defined(EPHEMERIS_PREFIX_SUMS)
    eph::signal::bitplane::Prefixes live_{};
#elif defined(EPHEMERIS_BITPLANE_TRACKING)
    eph::signal::bitplane::Planes live_{};
#else
    eph::signal::Epoch live_{};
#endif
    std::atomic<Slot> slot_{Slot::need_epoch};
    std::atomic<uint32_t> generation_{0}, occupied_{0};
    std::atomic<bool> offer_ready_{false};
    uint32_t snapshot_generation_{}, offer_generation_{};
    Detection offer_{};
    // The following five fields belong to the acquisition worker.
    eph::signal::Acquisition acquisition_{};
    eph::signal::Confirmation confirmation_{};
    Detection candidate_{};
    std::array<float, 32> last_carrier_{};
    uint32_t worker_generation_{}, warm_mask_{};
    uint32_t priority_mask_{};
    unsigned next_prn_ = 1;
    Search stage_ = Search::select;
    Status status_{};
    bool running_{}, have_clock_{};
    uint64_t next_sample_{}, clock_sample_{}, report_at_{};
    double clock_tow_{};
    void clear_channels();
    void navigate(uint64_t sample);
};
} // namespace ephemeris
