// SPDX-License-Identifier: Apache-2.0
#include "receiver/app.hpp"

#include <array>
#include <cassert>
#include <cstring>

int main() {
    static ephemeris::Receiver rx;
    std::array<uint8_t, eph::signal::bytes_per_ms> noise{};
    std::fill(noise.begin(), noise.end(), 0x39);
    ephemeris::WarmState saved;
    rx.save_warm(saved);
    assert(rx.restore_warm(saved) && !rx.status().fix.valid);
    saved.bytes[20] ^= 1;
    assert(!rx.restore_warm(saved));
    assert(!rx.work_acquisition());
    for (unsigned i=0; i<200; ++i)
        rx.process(noise.data(), uint64_t(i)*eph::signal::raw_per_ms, false);
    assert(rx.status().sequence == 1 && !rx.status().fix.valid);
    assert(rx.status().sample == 200ULL*eph::signal::raw_per_ms && !rx.status().gaps);
    const uint64_t resumed = 205ULL*eph::signal::raw_per_ms;
    rx.process(noise.data(), resumed, false);
    assert(rx.status().sequence==2 && rx.status().gaps==1 && !rx.status().locked_mask);
    char line[512];
    assert(ephemeris::format_status(line,sizeof(line),rx.status()) > 0);
    assert(std::strstr(line,",0,-1,") != nullptr);
    for (unsigned i=1; i<200; ++i)
        rx.process(noise.data(), resumed+uint64_t(i)*eph::signal::raw_per_ms,false);
    assert(rx.status().sequence==3 && !rx.status().fix.valid);
}
