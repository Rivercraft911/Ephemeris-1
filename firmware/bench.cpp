// SPDX-License-Identifier: Apache-2.0
#include "board.hpp"
#include "capture.hpp"
#include "pattern_source.hpp"
#include "usb.hpp"
#include "pico/stdlib.h"
#include <array>
#include <cstdio>
#include <cstring>

namespace {
constexpr unsigned n_samples = board::samples_per_ms * 10;
static_assert(n_samples % 32 == 0);
struct Snapshot {
    char magic[4] = {'E','I','Q','1'};
    uint32_t fs = board::sample_hz;
    int32_t intermediate_hz = board::if_hz;
    uint32_t format = 1;
    uint64_t first = 0, length = n_samples;
    uint32_t words[n_samples / 8];
} snapshot;
static_assert(offsetof(Snapshot, words) == 32 && sizeof(Snapshot) == 32 + n_samples / 2);

alignas(4) uint32_t planes[4][n_samples / 32];
capture::Lane packed, plane[4];
bool take(bool compare, bool internal, uint32_t& elapsed_us) {
    constexpr unsigned internal_base = 8;
    static bool source_started = false;
    if (internal && !source_started) {
        start_pattern_source(pio2, internal_base);
        source_started = true;
    }
    unsigned base = internal ? internal_base : board::data_base;
    packed.select_bus(base, base + 4);
    if (compare) for (unsigned i = 0; i < 4; ++i) plane[i].select_bus(base+i, base+4);
    packed.arm(snapshot.words, n_samples / 8);
    if (compare)
        for (unsigned i = 0; i < 4; ++i) plane[i].arm(planes[i], n_samples / 32);
    // RP2350 starts the four PIO0 lanes and the PIO1 nibble lane on the same cycle.
    uint64_t start = time_us_64();
    if (compare) pio_enable_sm_multi_mask_in_sync(pio1, 0xf, 1, 0);
    else pio_sm_set_enabled(pio1, 0, true);
    bool done;
    do {
        done = packed.finished();
        if (compare) for (auto& l : plane) done &= l.finished();
        usb::poll();
    } while (!done && time_us_64() - start < 100000);
    elapsed_us = uint32_t(time_us_64() - start);
    done &= !packed.stalled();
    packed.stop();
    if (compare) for (auto& l : plane) { done &= !l.stalled(); l.stop(); }
    return done;
}
unsigned compare_planes() {
    unsigned errors = 0;
    for (unsigned i = 0; i < n_samples; ++i) {
        unsigned nibble = (snapshot.words[i / 8] >> (4 * (i % 8))) & 15;
        unsigned reconstructed = 0;
        for (unsigned b = 0; b < 4; ++b)
            reconstructed |= ((planes[b][i / 32] >> (i % 32)) & 1u) << b;
        errors += nibble != reconstructed;
    }
    return errors;
}
}
int main() {
    board::configure_frontend();
    usb::init();
    packed.init(pio1, 0, board::data_base, 4, true);
    for (unsigned i = 0; i < 4; ++i)
        plane[i].init(pio0, i, board::data_base + i, 1, true);
    const uint8_t* output = nullptr;
    size_t remaining = 0;
    char response[120], cmd[40];
    for (;;) {
        usb::poll();
        if (!usb::connected()) { remaining = 0; continue; }
        if (remaining) {
            size_t n = usb::write(output, remaining);
            output += n; remaining -= n;
            continue;
        }
        if (!usb::command(cmd, sizeof(cmd))) continue;
        bool internal = std::strncmp(cmd, "self", 4) == 0;
        const char* mode = internal ? cmd + 4 : cmd;
        bool compare = std::strcmp(mode, "compare") == 0;
        if (!compare && std::strcmp(mode, "capture") != 0) {
            std::snprintf(response, sizeof(response), "ERR expected capture or compare\n");
        } else {
            uint32_t elapsed;
            if (!take(compare, internal, elapsed)) {
                std::snprintf(response, sizeof(response), "ERR clock timeout or FIFO stall\n");
            } else if (compare) {
                unsigned errors = compare_planes();
                std::snprintf(response, sizeof(response),
                    "%s samples=%u mismatches=%u clock=%s elapsed_us=%u\n",
                    errors ? "FAIL" : "PASS", n_samples, errors, internal ? "internal" : "external", unsigned(elapsed));
            } else {
                output = reinterpret_cast<const uint8_t*>(&snapshot);
                remaining = sizeof(snapshot);
                continue;
            }
        }
        output = reinterpret_cast<const uint8_t*>(response);
        remaining = std::strlen(response);
    }
}
