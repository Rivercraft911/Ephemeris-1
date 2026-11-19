// SPDX-License-Identifier: Apache-2.0
#include "receiver/app.hpp"

#include <array>
#include <cassert>
#include <cstring>

#ifdef EPHEMERIS_FLIGHT_HEALTH
namespace {
namespace sig = eph::signal;
using Packed = std::array<uint8_t, sig::bytes_per_ms>;

void broadcast_health() {
    ephemeris::Receiver rx;
    unsigned ms = 0, first_release = 0;
    // Parity-coded SF1 from the independent ICD equations in tools/sky_signal.py.
    // Changing word 3 to 0x03900021 makes the broadcast health field zero.
    constexpr std::array<uint32_t, 10> unhealthy{{0x22c00012,0x007d21ec,0x03903f1d,
        0x3fffffd6,0x29,0x3fffffd6,0x29,0x3fffffd6,0x29,0x3fffff20}};
    auto seed = [&](unsigned prn) {
        sig::Detection hit{};
        hit.prn = prn; hit.detected = true; hit.first_sample = uint64_t(ms)*sig::raw_per_ms;
        hit.code_phase_chips = 37*((prn-1)%8);
        return rx.seed_channel(hit);
    };
    for (unsigned group = 0; group < 4; ++group) {
        std::array<sig::Code, 8> codes;
        for (unsigned k = 0; k < codes.size(); ++k) {
            unsigned prn = 1 + 8*group + k;
            assert(sig::ca_code(prn,codes[k]) && seed(prn));
        }
        // Eight zero-Doppler transmitters share one data bit. Generate both
        // ADC polarities once, then let the production decoders find bit sync.
        std::array<Packed,2> symbols{};
        for (unsigned n = 0; n < sig::raw_per_ms; ++n) {
            int sum = 0;
            for (unsigned k = 0; k < codes.size(); ++k)
                sum += codes[k][(n/16+37*k)%sig::code_length];
            for (unsigned bit = 0; bit < 2; ++bit) {
                const int a = bit ? -sum : sum;
                const int re[] = {a,-a,-a,a}, im[] = {a,a,-a,-a};
                auto adc = [](int x) { return (x<0 ? 2u : 0u) | (std::abs(x)>1 ? 1u : 0u); };
                const unsigned v = 4*adc(re[n%4]) + adc(im[n%4]);
                symbols[bit][n/2] |= v << (4*(n%2));
            }
        }
        auto send_bit = [&](unsigned bit) {
            for (unsigned n = 0; n < 20; ++n, ++ms)
                rx.process(symbols[bit].data(),uint64_t(ms)*sig::raw_per_ms,false);
        };
        auto send_frame = [&](uint32_t word3) {
            auto words = unhealthy;
            words[2] = word3;
            for (uint32_t word : words) for (int b = 29; b >= 0; --b) send_bit((word>>b)&1);
        };
        for (unsigned n = 0; n < 50; ++n) send_bit(n%2);
        const uint32_t occupied = 0xffu << (8*group);
        assert(rx.status().locked_mask == occupied); // no SF1
        if (group == 0) {
            send_frame(unhealthy[2]^1u); // corrupt parity on the unhealthy SF1
            send_bit(0);
            assert(rx.status().locked_mask == occupied && rx.subframes(0)==0);
            send_frame(0x03900021);
            send_bit(0);
            assert(rx.status().locked_mask == occupied && rx.subframes(0)==1);
        }
        send_frame(unhealthy[2]);
        send_bit(0); // finish the last code-aligned bit on each channel
        assert(!rx.status().active_mask && !rx.status().locked_mask);
        if (group == 0) first_release = ms;
        assert(!seed(1+8*group)); // a queued offer cannot override the health hold
    }
    assert(!rx.work_acquisition()); // all 32 are deferred, one snapshot is ready
    Packed quiet{};
    for (; ms < first_release+59000; ++ms)
        rx.process(quiet.data(),uint64_t(ms)*sig::raw_per_ms,false);
    assert(!seed(1));
    for (; ms < first_release+60500; ++ms)
        rx.process(quiet.data(),uint64_t(ms)*sig::raw_per_ms,false);
    assert(seed(1) && !seed(32)); // each PRN has its own retry time
    ms = 0;
    rx.process(quiet.data(),0,true);
    assert(rx.status().gaps == 1 && !rx.status().active_mask && seed(32));
}
}
#endif

int main() {
#ifdef EPHEMERIS_FLIGHT_HEALTH
    broadcast_health();
#endif
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
