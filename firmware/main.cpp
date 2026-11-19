// SPDX-License-Identifier: Apache-2.0
#include "board.hpp"
#include "capture.hpp"
#include "usb.hpp"
#include "receiver/app.hpp"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
ephemeris::Receiver receiver;
ephemeris::WarmState __uninitialized_ram(retained);
constexpr uint32_t warm_cookie = 0x45505731;  // EPW1
bool warm_restored = false;
capture::Stream stream;
alignas(4) uint32_t block[board::words_per_ms];
struct Timing {
    uint32_t longest_us{}, t_bins[64]{}, counts{}, dropped_lines{};
    void record(uint32_t us) {
        longest_us = std::max(longest_us, us);
        ++t_bins[std::min<uint32_t>(63u, us / 50)]; ++counts;
    }
    unsigned percentile95_bin() const {
        uint64_t sum = 0;
        for (unsigned b = 0; b < 64; ++b) {
            sum += t_bins[b];
            if (sum * 100 >= uint64_t(counts) * 95) return b;
        }
        return 63;
    }
} timing;

class SerialLine {
public:
    void service() {
        if (used_ > offset_) offset_ += usb::write(buf_ + offset_, used_ - offset_);
        if (offset_ == used_) clear();
    }
    char* begin() {
        if (used_) { ++timing.dropped_lines; return nullptr; }
        return buf_;
    }
    void finish(int n) {
        if (n < 0 || n > int(sizeof(buf_) - 2)) { ++timing.dropped_lines; return; }
        buf_[n] = '\n';
        used_ = size_t(n) + 1;
    }
    void clear() { used_ = offset_ = 0; }
    bool busy() const { return used_ != 0; }
    static constexpr unsigned capacity = 512;
private:
    char buf_[capacity]{};
    size_t used_{}, offset_{};
};
struct Measurements {
    struct Row {
        double tow_s;
        float doppler_hz;
        unsigned prn;
        bool locked, timed;
    };
    Row rows[ephemeris::Receiver::channels]{};
    uint64_t sample{};
    unsigned count{}, cursor{};
    bool pending{};

    void capture(uint64_t end, bool clock_alive) {
        if (pending) timing.dropped_lines += count - cursor + 1;
        sample = end;
        count = cursor = 0;
        if (clock_alive) for (unsigned i = 0; i < receiver.channels; ++i) {
            const auto& channel = receiver.channel(i);
            if (!channel.active || channel.first_sample + board::samples_per_ms != end) continue;
            auto& row = rows[count++];
            row.prn = channel.prn;
            row.locked = channel.locked;
            row.doppler_hz = channel.doppler_hz;
            row.timed = row.locked && receiver.channel_tx_tow(i, double(end), row.tow_s);
        }
        pending = true;
    }
    void service(SerialLine& line) {
        if (!pending || line.busy()) return;
        char* text = line.begin();
        if (cursor == count) {
            line.finish(std::snprintf(text, line.capacity, "#obs_end,%llu,%u",
                                      (unsigned long long)sample, count));
            pending = false;
            return;
        }
        const auto& row = rows[cursor++];
        char time[32] = "";
        if (row.timed) std::snprintf(time, sizeof(time), "%.9f", row.tow_s);
        line.finish(std::snprintf(text, line.capacity, "#obs,%llu,%u,%u,%.4f,%s",
            (unsigned long long)sample, row.prn, unsigned(row.locked), double(row.doppler_hz), time));
    }
};
// Keep both hardware guards enabled. Navigation's on-stack least-squares workspace,
// call chain and USB interrupts require more space than a default scratch bank.
alignas(32) uint32_t tracking_stack[4096];
void tracking_worker() {
    board::configure_frontend();
    usb::init();
    stream.start();
    static SerialLine line;
    static Measurements measurements;
    uint32_t last_receiver_seq = 0, output_sequence = 0;
    uint64_t last_status_us = time_us_64(), end_sample = 0;
    bool attached = false, stream_measurements = false;
    for (;;) {
        uint64_t first{};
        bool gap{};
        if (stream.read(block, first, gap)) {
            uint64_t start = time_us_64();
            receiver.process(reinterpret_cast<const uint8_t*>(block), first, gap);
            end_sample = first + board::samples_per_ms;
            timing.record(uint32_t(time_us_64() - start));
        }
        usb::poll();
        if (!usb::connected()) {
            attached = stream_measurements = measurements.pending = false;
            line.clear();
        }
        else if (!attached) {
            char* text = line.begin();
            line.finish(std::snprintf(text, line.capacity, "%s", ephemeris::status_header));
            attached = true;
        }
        line.service();

        char cmd[24];
        const bool command = usb::command(cmd, sizeof(cmd));
        bool measure_now = false;
        if (command) {
            measure_now = !std::strcmp(cmd, "observables") || !std::strcmp(cmd, "observables on");
            if (!std::strcmp(cmd, "observables on")) stream_measurements = true;
            if (!std::strcmp(cmd, "observables off")) stream_measurements = measurements.pending = false;
        }
        if (command && (!std::strcmp(cmd, "warmboot") || !std::strcmp(cmd, "coldboot"))) {
            watchdog_hw->scratch[0] = 0;
            if (!std::strcmp(cmd, "warmboot")) {
                receiver.save_warm(retained);
                __dmb();
                watchdog_hw->scratch[0] = warm_cookie;
            }
            watchdog_reboot(0, 0, 0);
            for (;;) tight_loop_contents();
        }
        if (command && std::strcmp(cmd, "stats") == 0) {
            if (char* text = line.begin()) {
                auto& s = stream.stats();
                line.finish(std::snprintf(text, line.capacity,
                    "#stats blocks=%llu missed=%llu fifo_stalls=%u clock_timeouts=%u "
                    "peak_backlog_bytes=%u process_max_us=%u process_p95_bin50us=%u "
                    "dropped_usb_lines=%u clock_alive=%u warm_restored=%u",
                    (unsigned long long)s.blocks, (unsigned long long)s.missed_blocks,
                    unsigned(s.fifo_stalls), unsigned(s.clock_timeouts), unsigned(s.peak_backlog_bytes),
                    unsigned(timing.longest_us), timing.percentile95_bin(), unsigned(timing.dropped_lines),
                    unsigned(stream.clock_alive()), unsigned(warm_restored)));
            }
        }
        uint64_t now = time_us_64();
        auto& status = receiver.status();
        bool updated = status.sequence != last_receiver_seq;
        // A missing sample clock still produces invalid heartbeats with the last sample index.
        if (updated || (!stream.clock_alive() && now - last_status_us >= 200000)) {
            last_receiver_seq = status.sequence;
            last_status_us = now;
            ++output_sequence;
            if (attached) {
                measure_now |= stream_measurements;
                if (char* text = line.begin()) {
                    auto outgoing = status;
                    if (!updated) {
                        outgoing.fix = {};
                        outgoing.active_mask = outgoing.locked_mask = outgoing.valid_nav_mask = 0;
                    }
                    outgoing.sequence = output_sequence;
                    line.finish(ephemeris::format_status(text, line.capacity, outgoing));
                }
            }
        }
        if (measure_now) measurements.capture(end_sample, stream.clock_alive());
        measurements.service(line);
    }
}
}
int main() {
    const bool warm_request = watchdog_caused_reboot() && watchdog_hw->scratch[0] == warm_cookie;
    watchdog_hw->scratch[0] = 0;
    if (warm_request) warm_restored = receiver.restore_warm(retained);
    multicore_launch_core1_with_stack(tracking_worker, tracking_stack, sizeof(tracking_stack));
    for (;;) if (!receiver.work_acquisition()) tight_loop_contents();
}
