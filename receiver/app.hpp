// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "navigation.hpp"
#include "signal.hpp"
#ifdef EPHEMERIS_COARSE_TRACKING
#include "coarse.hpp"
#elif defined(EPHEMERIS_BITPLANE_TRACKING)
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
    // Install before processing begins. The notification must arrange exactly
    // one worker to run work_tracking(); wait may sleep until it completes.
    using WorkerHook = void (*)(void*);
    void set_tracking_worker(WorkerHook notify, WorkerHook wait, void* context);
    bool work_tracking();
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
    WorkerHook tracking_notify_{}, tracking_wait_{};
    void* tracking_context_{};
    std::atomic<bool> tracking_pending_{false};
    void track_alternating(unsigned first);
    enum class Slot : unsigned { need_epoch, ready, idle };
    enum class Search : unsigned { select, collect, scanning, survey, confirm };
    std::array<eph::signal::Tracker, channels> trackers_{};
    std::array<eph::signal::TrackingResult, channels> results_{};
    std::array<eph::nav::LnavDecoder, channels> decoders_{};
    std::array<eph::nav::Ephemeris, 32> cache_{};
    std::array<unsigned, channels> prns_{};
    eph::signal::Epoch snapshot_{};
#if defined(EPHEMERIS_COARSE_TRACKING)
    eph::signal::coarse::Epoch live_{};
#elif defined(EPHEMERIS_PREFIX_SUMS)
    eph::signal::bitplane::Prefixes live_{};
#elif defined(EPHEMERIS_BITPLANE_TRACKING)
    eph::signal::bitplane::Planes live_{};
#else
    eph::signal::Epoch live_{};
#endif
#ifdef EPHEMERIS_FLIGHT_HEALTH
    std::atomic<uint32_t> deferred_{0}; // capture owner writes, acquisition worker reads
    std::array<uint16_t, 32> retry_report_{}; // capture owner only
#endif
    std::atomic<Slot> slot_{Slot::need_epoch};
    std::atomic<uint32_t> generation_{0}, occupied_{0};
    std::atomic<bool> offer_ready_{false};
    uint32_t snapshot_generation_{}, offer_generation_{};
    Detection offer_{};
    // Search state belongs to the acquisition worker.
    eph::signal::Acquisition acquisition_{};
    eph::signal::Confirmation confirmation_{};
    Detection candidate_{};
    std::array<float, 32> last_carrier_{};
    uint32_t worker_generation_{}, warm_mask_{};
    uint32_t priority_mask_{};
    unsigned search_index_{}; // index in the cold acquisition schedule
    Search stage_ = Search::select;
    bool may_refresh_{}; // acquisition worker, one fresh local search per cold or warm candidate
    Status status_{};
#ifdef EPHEMERIS_DOPPLER_POSITION
    eph::nav::PositionSmoother position_smoother_{};
#endif
#ifdef EPHEMERIS_VELOCITY_FIT
    eph::nav::VelocityFit velocity_fit_{};
#endif
    bool running_{}, have_clock_{};
    uint64_t next_sample_{}, clock_sample_{}, report_at_{};
    double clock_tow_{};
    void clear_channels();
    void navigate(uint64_t sample);
};
} // namespace ephemeris
