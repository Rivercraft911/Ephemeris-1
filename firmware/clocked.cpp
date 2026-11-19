// SPDX-License-Identifier: Apache-2.0
// Bench only. Reconstruct a compact IF recording under a real 1 kHz clock.
#include "receiver/app.hpp"
#include "replay_crc.hpp"
#include "tracking_irq.hpp"
#include "usb.hpp"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
namespace sig = eph::signal;
constexpr unsigned epoch_bytes = 512, ring_bytes = 64 * epoch_bytes;
constexpr unsigned slot_count = 4;
uint8_t __uninitialized_ram(input_ring)[ring_bytes];
alignas(32) uint32_t __uninitialized_ram(worker_stack)[4096];
// The compact epoch has two padding pairs. Expand the last byte uniformly;
// the receiver only reads the first 2046 half chips.
alignas(4) uint32_t raw[4 * epoch_bytes];
static_assert(sig::raw_per_ms / 8 == 4 * epoch_bytes - 2);
ephemeris::Receiver receiver;
struct Line { char text[448]; unsigned length{}; };
Line lines[slot_count];
std::atomic<unsigned> received{}, consumed{}, queued{}, sent{};
std::atomic<bool> started{}, done{}, detached{};
unsigned epochs, line_offset, peak_backlog, starved, dropped_status, processed;
uint32_t max_late_us, worst_epoch, crc = ~uint32_t{};
uint64_t start_us, elapsed_us;

constexpr uint32_t if_word(unsigned pair) {
    const bool i = pair & 1u, q = pair & 2u;
    auto sample = [](bool neg_i, bool neg_q) { return (neg_i ? 8u : 0u) | (neg_q ? 2u : 0u); };
    const unsigned cycle = sample(i,q) | (sample(!q,i) << 4) |
                           (sample(!i,!q) << 8) | (sample(q,!i) << 12);
    return cycle | (cycle << 16);
}
constexpr auto make_waveforms() {
    std::array<std::array<uint32_t, 4>, 256> words{};
    for (unsigned byte = 0; byte < words.size(); ++byte)
        for (unsigned pair = 0; pair < 4; ++pair)
            words[byte][pair] = if_word((byte >> (2 * pair)) & 3);
    return words;
}
constexpr auto waveform = make_waveforms();
void expand(const uint8_t* compact) {
    for (unsigned b = 0; b < epoch_bytes; ++b) {
        const uint8_t v = compact[b];
        crc = (crc >> 8) ^ replay_crc::table[(crc ^ v) & 255];
        std::memcpy(raw + 4*b, waveform[v].data(), sizeof(waveform[v]));
    }
}
// One USB owner. During replay the core 0 timer services the endpoint while
// its foreground runs acquisition. The receiver and output producer live on core 1.
void pump() {
    usb::poll();
    if (!usb::connected()) { detached.store(true, std::memory_order_release); return; }
    const auto w = received.load(std::memory_order_relaxed);
    const unsigned offset = w % ring_bytes;
    const auto space = ring_bytes - (w - consumed.load(std::memory_order_acquire));
    const auto room = std::min({space, ring_bytes - offset, epochs*epoch_bytes - w});
    if (room) {
        const unsigned bytes = usb::read(input_ring + offset, room);
        if (bytes) received.store(w + bytes, std::memory_order_release);
    }
    const auto next = sent.load(std::memory_order_relaxed);
    if (next == queued.load(std::memory_order_acquire)) return;
    const auto& line = lines[next % slot_count];
    line_offset += usb::write(line.text + line_offset, line.length - line_offset);
    if (line_offset == line.length) {
        line_offset = 0;
        sent.store(next + 1, std::memory_order_release);
    }
}
bool timer_pump(repeating_timer*) { pump(); return true; }

void emit(const ephemeris::Status& status, uint64_t now) {
    const auto w = queued.load(std::memory_order_relaxed);
    if (w - sent.load(std::memory_order_acquire) == slot_count) { ++dropped_status; return; }
    auto& line = lines[w % slot_count];
    const int prefix = std::snprintf(line.text, sizeof(line.text), "S %" PRIu64 " ", now - start_us);
    const int length = ephemeris::format_status(line.text + prefix, sizeof(line.text) - prefix,
                                               status);
    if (length < 0 || prefix + length + 1 >= int(sizeof(line.text))) { ++dropped_status; return; }
    unsigned used = prefix + length;
#ifdef EPHEMERIS_MOTION_AVERAGE
    const auto& raw_m = status.raw_position_m;
    const int extra = std::snprintf(line.text + used, sizeof(line.text) - used, ",%.3f,%.3f,%.3f,%u",
                                    raw_m[0], raw_m[1], raw_m[2], unsigned(status.motion_samples));
    if (extra < 0 || used + extra + 1 >= sizeof(line.text)) { ++dropped_status; return; }
    used += extra;
#endif
    line.text[used] = '\n';
    line.length = used + 1;
    queued.store(w + 1, std::memory_order_release);
}
void process() {
    while (!started.load(std::memory_order_acquire)) tight_loop_contents();
    for (; processed < epochs; ++processed) {
        const uint64_t deadline = start_us + (uint64_t(processed) + 1)*1000;
        busy_wait_until(from_us_since_boot(deadline));
        if (detached.load(std::memory_order_acquire)) break;
        const unsigned offset = processed * epoch_bytes;
        if (received.load(std::memory_order_acquire) - offset < epoch_bytes) { ++starved; break; }
        const auto begin = time_us_64();
        peak_backlog = std::max(peak_backlog, unsigned((begin - deadline)/1000) + 1);
        expand(input_ring + offset % ring_bytes);
        consumed.store(offset + epoch_bytes, std::memory_order_release);
        receiver.process(reinterpret_cast<const uint8_t*>(raw), uint64_t(processed)*sig::raw_per_ms, false);
        const auto end = time_us_64();
        const auto late = uint32_t(end - deadline);
        if (late > max_late_us) {
            max_late_us = late;
            worst_epoch = processed + 1;
        }
        if ((processed + 1) % 200 == 0) emit(receiver.status(), end);
    }
    elapsed_us = time_us_64() - start_us;
    done.store(true, std::memory_order_release);
    for (;;) tight_loop_contents();
}
bool write_line(const char* text) {
    size_t left = std::strlen(text);
    const auto timeout = make_timeout_time_ms(2000);
    while (left && !time_reached(timeout)) {
        usb::poll();
        if (!usb::connected()) return false;
        const auto n = usb::write(text, left);
        text += n;
        left -= n;
    }
    return left == 0;
}
void run(uint32_t expected_crc) {
    char header[384];
#ifdef EPHEMERIS_MOTION_AVERAGE
    std::snprintf(header, sizeof(header),
                  "READY CLK1 motion_window=%u\nH %s,raw_x_m,raw_y_m,raw_z_m,motion_samples\n",
                  eph::nav::MotionAverage::window, ephemeris::status_header);
#else
    std::snprintf(header, sizeof(header), "READY CLK1\nH %s\n", ephemeris::status_header);
#endif
    if (!write_line(header)) return;
    const unsigned prefill = std::min(epochs*epoch_bytes, ring_bytes);
    const auto timeout = make_timeout_time_ms(5000);
    while (received.load() < prefill && !time_reached(timeout) && !detached.load()) pump();
    if (received.load() < prefill) { write_line("ERR input prefill\n"); return; }
    repeating_timer service;
    // A tracking doorbell can occupy core 0 for most of an epoch. Let the
    // endpoint and its service timer keep accepting packets during that work.
    irq_set_priority(USBCTRL_IRQ, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_priority(hardware_alarm_get_irq_num(alarm_pool_hardware_alarm_num(alarm_pool_get_default())),
                     PICO_HIGHEST_IRQ_PRIORITY + 0x40);
    if (!add_repeating_timer_us(-64, timer_pump, nullptr, &service)) {
        write_line("ERR USB service timer\n"); return;
    }
    start_us = time_us_64();
    started.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) receiver.work_acquisition();
    cancel_repeating_timer(&service);
    const auto flush_timeout = make_timeout_time_ms(1500);
    while (sent.load() != queued.load() && !detached.load() && !time_reached(flush_timeout)) pump();
    const uint32_t checksum = ~crc;
    const bool ok = processed == epochs && max_late_us < 4000 && !starved && !dropped_status &&
                    sent.load() == queued.load() && checksum == expected_crc;
    char summary[192];
    std::snprintf(summary, sizeof(summary), "%s epochs=%u elapsed_us=%" PRIu64
                  " late_us=%" PRIu32 " worst_epoch=%" PRIu32 " starved=%u peak_backlog=%u crc=%08" PRIx32 "\n",
                  ok ? "PASS" : "FAIL", processed, elapsed_us, max_late_us, worst_epoch, starved, peak_backlog, checksum);
    write_line(summary);
}
}
int main() {
    usb::init();
    board::install_tracking_irq(receiver);
    multicore_launch_core1_with_stack(process, worker_stack, sizeof(worker_stack));
    bool used = false;
    for (;;) {
        usb::poll();
        char command[40];
        if (!usb::command(command, sizeof(command))) continue;
        if (used) { write_line("ERR reload for a cold run\n"); continue; }
        if (std::strncmp(command, "clocked ", 8)) { write_line("ERR expected clocked\n"); continue; }
        char* end;
        const auto count = std::strtoul(command+8, &end, 10);
        if (!count || count > 600000 || *end != ' ' || std::strlen(end+1) != 8 ||
            std::strspn(end+1,"0123456789abcdefABCDEF") != 8) {
            write_line("ERR expected epoch count and CRC\n"); continue;
        }
        const auto checksum = std::strtoul(end+1, nullptr, 16);
        used = true;
        epochs = count;
        run(checksum);
    }
}
