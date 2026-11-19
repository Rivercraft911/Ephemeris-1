// SPDX-License-Identifier: Apache-2.0
// Bench-only image: process the supplied recording in real time while independent PIO
// and DMA traffic supplies the epoch cadence. No laboratory hints enter ephemeris_pico.
#include "board.hpp"
#include "capture.hpp"
#include "usb.hpp"
#include "receiver/app.hpp"
#include "tests/fixtures/rollover.hpp"
#include "tracking_irq.hpp"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
ephemeris::Receiver receiver;
capture::Stream stream;
alignas(32) uint32_t tracking_stack[4096];
alignas(4) uint32_t input[board::words_per_ms], replay[board::words_per_ms];
std::atomic<uint32_t> work_count{0};
std::atomic<bool> suspend_search{false}, search_idle{false};
constexpr unsigned epoch_target = 1000;
uint32_t durations[epoch_target];

void reply(const char* line) {
    const size_t length = std::strlen(line);
    size_t sent = 0;
    const uint64_t start = time_us_64();
    while (usb::connected() && sent < length && time_us_64() - start < 2000000) {
        usb::poll(); sent += usb::write(line + sent, length - sent);
    }
}

// GP8 is a bench-only, on-chip clock. It never drives the external HOST_CLK net on GP6.
constexpr unsigned internal_clock = 8;
unsigned internal_sm = 0;
void start_internal_clock() {
    PIO pio = pio2;
    unsigned sm = pio_claim_unused_sm(pio, true);
    internal_sm = sm;
    const uint16_t instructions[] = {uint16_t(pio_encode_set(pio_pins, 0)),
                                     uint16_t(pio_encode_set(pio_pins, 1))};
    const pio_program_t program{instructions, 2, -1};
    unsigned offset = pio_add_program(pio, &program);
    auto cfg = pio_get_default_sm_config();
    sm_config_set_wrap(&cfg, offset, offset + 1);
    sm_config_set_set_pins(&cfg, internal_clock, 1);
    sm_config_set_clkdiv(&cfg, float(clock_get_hz(clk_sys)) / (2 * board::sample_hz));
    pio_gpio_init(pio, internal_clock);
    pio_sm_set_consecutive_pindirs(pio, sm, internal_clock, 1, true);
    pio_sm_init(pio, sm, offset, &cfg);
    pio_sm_set_enabled(pio, sm, true);
}
void check_clock_faults() {
    stream.select_clock(internal_clock);
    auto next = [](uint64_t& first, bool& gap) {
        const auto start = time_us_64();
        while (time_us_64()-start < 50000) {
            if (stream.read(input, first, gap)) return true;
            usb::poll();
        }
        return false;
    };
    bool good[2]{}, gap = false;
    uint64_t before = 0, after = 0;
    for (unsigned level = 0; level < 2; ++level) {
        if (!next(before, gap)) break;
        const unsigned faults = stream.stats().clock_timeouts;
        pio_sm_set_enabled(pio2, internal_sm, false);
        pio_sm_set_pins_with_mask(pio2, internal_sm, level << internal_clock, 1u << internal_clock);
        busy_wait_us_32(10);
        pio_sm_set_enabled(pio2, internal_sm, true);
        busy_wait_us_32(10);  // Resume the input before the first CPU poll.
        good[level] = next(after, gap) && gap && after >= before + board::samples_per_ms &&
                      stream.stats().clock_timeouts == faults + 1;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "%s stopped_low=%u stopped_high=%u hold_us=10\n",
                  good[0] && good[1] ? "PASS" : "FAIL", unsigned(good[0]), unsigned(good[1]));
    reply(line);
}
void check_fix_timing() {
    std::array<eph::nav::Ephemeris,6> eph{};
    std::array<eph::nav::Observation,6> obs{};
    eph::fixture::rollover_observations(eph, obs);
    eph::nav::Fix f;
    constexpr unsigned repeats = 20;
    uint32_t total = 0, worst = 0;
    bool valid = true;
    for (unsigned i = 0; i < repeats; ++i) {
        const auto start = time_us_32();
        valid &= eph::nav::solve_fix(obs.data(), obs.size(), f, .0021);
        const auto us = time_us_32()-start;
        total += us;
        worst = std::max(worst,us);
        usb::poll();
    }
    double dp2 = 0, dv2 = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const auto dp = f.position_m[i]-(eph::fixture::at_zero[i]+.002*eph::fixture::velocity[i]);
        const auto dv = f.velocity_mps[i]-eph::fixture::velocity[i];
        dp2 += dp*dp;
        dv2 += dv*dv;
    }
    valid &= f.valid && f.week_mod1024==303 && std::abs(f.tow_s-.002)<1e-9 && dp2<.04 && dv2<.000016;
    char line[160];
    std::snprintf(line,sizeof(line),"%s sv=6 repeats=%u mean_us=%u max_us=%u position_m=%.5f velocity_mps=%.6f\n",
                  valid ? "PASS" : "FAIL",repeats,total/repeats,worst,std::sqrt(dp2),std::sqrt(dv2));
    reply(line);
}
void check_continuity(unsigned seconds) {
    stream.select_clock(internal_clock);
    const auto initial = stream.stats();
    const auto start = time_us_64();
    const unsigned target = 1000 * seconds;
    unsigned count = 0, unexpected_gaps = 0;
    uint64_t previous = 0, first = 0;
    while (count < target && usb::connected() && time_us_64() - start < (seconds + 2u)*1000000ull) {
        uint64_t sample;
        bool gap;
        usb::poll();
        if (!stream.read(input, sample, gap)) continue;
        if (!count) first = sample;
        else if (gap || sample != previous + board::samples_per_ms) ++unexpected_gaps;
        previous = sample;
        ++count;
    }
    const auto elapsed = time_us_64() - start;
    stream.stop();
    const auto final = stream.stats();
    const auto missed = final.missed_blocks - initial.missed_blocks;
    const auto stalls = final.fifo_stalls - initial.fifo_stalls;
    const auto timeouts = final.clock_timeouts - initial.clock_timeouts;
    const bool passed = count == target && !unexpected_gaps && !missed && !stalls && !timeouts;
    char line[256];
    std::snprintf(line, sizeof(line),
        "%s epochs=%u samples=%llu elapsed_us=%llu first=%llu last=%llu gaps=%u missed=%llu stalls=%u clock_timeouts=%u\n",
        passed ? "PASS" : "FAIL", count, (unsigned long long)count * board::samples_per_ms,
        (unsigned long long)elapsed, (unsigned long long)first, (unsigned long long)previous,
        unexpected_gaps, (unsigned long long)missed, stalls, timeouts);
    reply(line);
}
bool measure_kernels(unsigned& prepare_us, unsigned& tracker_us) {
    namespace sig = eph::signal;
#if defined(EPHEMERIS_COARSE_TRACKING)
    static sig::coarse::Epoch epoch;
#elif defined(EPHEMERIS_PREFIX_SUMS)
    static sig::bitplane::Prefixes epoch;
#elif defined(EPHEMERIS_BITPLANE_TRACKING)
    static sig::bitplane::Planes epoch;
#else
    static sig::Epoch epoch;
#endif
    static sig::Tracker tracker;
    constexpr unsigned count = 48, settle = 8;
    sig::Detection seed{};
    seed.prn = 1;
    seed.detected = true;
    tracker.start(seed);
    bool locked = false;
    for (unsigned n = 0; n < count + settle; ++n) {
        sig::PackedBlock block{uint64_t(n) * board::samples_per_ms,
                               reinterpret_cast<const uint8_t*>(replay), sizeof(replay)};
        uint32_t begin = time_us_32();
#if defined(EPHEMERIS_COARSE_TRACKING)
        sig::coarse::preprocess(block, epoch);
#elif defined(EPHEMERIS_BITPLANE_TRACKING)
        sig::bitplane::from_packed(block, epoch);
#else
        sig::preprocess(block, epoch);
#endif
        uint32_t middle = time_us_32();
        const auto result = tracker.update(epoch);
        uint32_t end = time_us_32();
        locked = result.locked;
        if (n >= settle) {
            prepare_us += middle - begin;
            tracker_us += end - middle;
        }
        usb::poll();
    }
    prepare_us = (prepare_us + count/2)/count;
    tracker_us = (tracker_us + count/2)/count;
    return locked;
}

enum class Mode { internal, external, offline };
void profile(unsigned channels, Mode mode) {
    reply("READY 8184\n");
    size_t read = 0;
    uint64_t start = time_us_64();
    while (read < sizeof(replay) && usb::connected() && time_us_64() - start < 2000000) {
        usb::poll();
        read += usb::read(reinterpret_cast<uint8_t*>(replay) + read, sizeof(replay) - read);
    }
    if (read != sizeof(replay)) { reply("ERR incomplete fixture\n"); return; }

    constexpr unsigned hot_start = 10;  // allow the loops to establish initial lock
    const bool offline = mode == Mode::offline;
    uint32_t worst = 0, maximum_active = 0;
    unsigned prepare_us = 0, tracker_us = 0, full_epochs = 0;
    bool kernel_locked = false;
    uint64_t total_us = 0;
    uint32_t expected_mask = (1u << channels) - 1, locked_at_end = 0, full_lock_epochs = 0;
    constexpr uint64_t first_epoch = 0;
    unsigned n = 1;  // Epoch zero initializes the receiver before the DMA cadence starts.
    stream.stop();
    suspend_search.store(offline, std::memory_order_release);
    if (offline) {
        while (!search_idle.load(std::memory_order_acquire)) tight_loop_contents();
        kernel_locked = measure_kernels(prepare_us, tracker_us);
    }
    receiver.process(reinterpret_cast<const uint8_t*>(replay), first_epoch, true);
    for (unsigned i = 0; i < channels; ++i) {
        eph::signal::Detection seed{};
        seed.prn = i + 1;
        seed.first_sample = first_epoch;
        seed.code_phase_chips = i * 37;
        seed.detected = true;
        if (!receiver.seed_channel(seed)) {
            suspend_search.store(false, std::memory_order_release);
            reply("ERR seeding channel\n");
            return;
        }
    }
    const auto before = stream.stats();
    const uint32_t work_start = work_count.load();
    if (!offline) stream.start_at(mode == Mode::external ? board::clock_pin : internal_clock,
                                 first_epoch + board::samples_per_ms);
    start = time_us_64();
    while (n < epoch_target && time_us_64() - start < (offline ? 120000000u : 6000000u)) {
        usb::poll();
        uint64_t first = uint64_t(n) * board::samples_per_ms;
        bool gap = false;
        if (!offline && !stream.read(input, first, gap)) continue;
        uint64_t tick = time_us_64();
        receiver.process(reinterpret_cast<const uint8_t*>(replay), first, gap);
        uint32_t us = time_us_64() - tick;
        if (n >= hot_start) { durations[full_epochs++] = us; total_us += us; }
        worst = std::max(worst, us);
        uint32_t active = 0, locked = 0;
        for (unsigned i = 0; i < receiver.channels; ++i) {
            const auto& result = receiver.channel(i);
            if (!result.prn) continue;
            uint32_t mask = 1u << (result.prn - 1);
            if (result.active) active |= mask;
            if (result.locked) locked |= mask;
        }
        maximum_active = std::max(maximum_active, uint32_t(__builtin_popcount(active)));
        locked_at_end = locked;
        if (n >= hot_start && locked == expected_mask) ++full_lock_epochs;
        ++n;
    }
    const auto& after = stream.stats();
    uint64_t lost = after.missed_blocks - before.missed_blocks;
    uint32_t stalls = after.fifo_stalls - before.fifo_stalls;
    uint32_t timeouts = after.clock_timeouts - before.clock_timeouts;
    uint32_t work = work_count.load() - work_start;
    std::sort(durations, durations + full_epochs);
    const unsigned p95 = full_epochs ? durations[(95*full_epochs+99)/100-1] : 0;
    bool passed = n == epoch_target && (offline ? kernel_locked : (!lost && !stalls && !timeouts && worst < 1000))
               && locked_at_end == expected_mask && maximum_active == channels
               && full_lock_epochs == epoch_target - hot_start
               && (offline || channels == receiver.channels || work > 0);
    char line[512];
    std::snprintf(line, sizeof(line),
        "%s clock=%s channels=%u epochs=%u first=%llu max_us=%u p95_us=%u steady_mean_us=%u missed=%llu "
        "stalls=%u clock_timeouts=%u max_active=%u locked_mask=%u full_lock_epochs=%u work_steps=%u "
        "isolated_prepare_mean_us=%u isolated_tracker_mean_us=%u isolated_locked=%u sys_hz=%u "
        "tracking_cores=%u core0_stack_untouched_bytes=%d\n",
        passed ? "PASS" : "FAIL", offline ? "offline" : mode == Mode::external ? "external" : "internal",
        channels, n, (unsigned long long)first_epoch, unsigned(worst), p95,
        full_epochs ? unsigned(total_us/full_epochs) : 0, (unsigned long long)lost, unsigned(stalls),
        unsigned(timeouts), unsigned(maximum_active), unsigned(locked_at_end), unsigned(full_lock_epochs),
        unsigned(work), prepare_us, tracker_us, unsigned(kernel_locked), unsigned(clock_get_hz(clk_sys)),
        board::tracking_cores, board::core0_stack_untouched());
    reply(line);
    for (unsigned i = 0; i < receiver.channels; ++i) {
        const auto& result = receiver.channel(i);
        if (!result.active) continue;
        std::snprintf(line, sizeof(line),
            "channel prn=%u sample=%llu phase_chips=%.6f doppler_hz=%.5f prompt_i=%.5f prompt_q=%.5f\n",
            result.prn, (unsigned long long)result.first_sample, result.code_phase_chips,
            double(result.doppler_hz), double(result.prompt_i), double(result.prompt_q));
        reply(line);
    }
    reply("END\n");
    suspend_search.store(false, std::memory_order_release);
}
void bench_worker() {
    usb::init();
    stream.start();
    start_internal_clock();
    for (;;) {
        usb::poll();
        char cmd[24];
        if (!usb::command(cmd, sizeof(cmd))) continue;
        if (!std::strcmp(cmd, "pvt")) { check_fix_timing(); continue; }
        if (!std::strcmp(cmd, "clocktest")) { check_clock_faults(); continue; }
        if (!std::strncmp(cmd, "continuity ", 11)) {
            char* end;
            const unsigned long seconds = std::strtoul(cmd + 11, &end, 10);
            if (!*end && seconds >= 1 && seconds <= 600) check_continuity(unsigned(seconds));
            else reply("ERR expected a duration of 1 to 600 seconds\n");
            continue;
        }
        for (unsigned channels : {1u, 4u, 6u, 8u}) {
            char expected[24];
            std::snprintf(expected, sizeof(expected), "profile %u", channels);
            if (!std::strcmp(cmd, expected)) { profile(channels, Mode::internal); break; }
            std::snprintf(expected, sizeof(expected), "external %u", channels);
            if (!std::strcmp(cmd, expected)) { profile(channels, Mode::external); break; }
            std::snprintf(expected, sizeof(expected), "offline %u", channels);
            if (!std::strcmp(cmd, expected)) { profile(channels, Mode::offline); break; }
            if (channels == 8) reply("ERR expected profile, external or offline, followed by 1, 4, 6 or 8\n");
        }
    }
}
}
int main() {
    board::install_tracking_irq(receiver);
    multicore_launch_core1_with_stack(bench_worker, tracking_stack, sizeof(tracking_stack));
    for (;;) {
        if (suspend_search.load(std::memory_order_acquire)) {
            search_idle.store(true, std::memory_order_release);
            while (suspend_search.load(std::memory_order_acquire)) tight_loop_contents();
            search_idle.store(false, std::memory_order_release);
        }
        if (receiver.work_acquisition()) work_count.fetch_add(1, std::memory_order_relaxed);
        else tight_loop_contents();
    }
}
