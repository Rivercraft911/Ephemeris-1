// SPDX-License-Identifier: Apache-2.0
#include "coarse.hpp"

#include <algorithm>
#include <cmath>

namespace eph::signal::coarse {
namespace {
constexpr int frac = 1 << 16;
constexpr int period = half_chips * frac;
constexpr double baseband_hz = raw_hz / 8;

// One byte holds two ADC samples. Rotate them by the exact Fs/4 IF and
// encode both axis sums in separate bytes. Four lookups set bit 7 exactly
// when the integrated axis is nonnegative, including ties.
constexpr auto adc_table() {
    std::array<std::array<std::uint16_t,256>,4> table{};
    constexpr int voltage[] = {1,3,-1,-3};
    for (int phase=0; phase<4; ++phase) {
        for (int pair=0; pair<256; ++pair) {
            int i=32, q=32;
            for (int n=0; n<2; ++n) {
                const int v = pair >> (4*n);
                const int x = voltage[(v>>2)&3], y = voltage[v&3];
                const int re[] = {x,y,-x,-y}, im[] = {y,-x,-y,x};
                i += re[(phase+n)&3]; q += im[(phase+n)&3];
            }
            table[phase][pair] = static_cast<std::uint16_t>(i + 256*q);
        }
    }
    return table;
}
constexpr auto adc = adc_table();

// Only the intra-group carrier slope is quantized (1/1024 turn/sample).
// Re-evaluate each group origin from the 32-bit phase accumulator. The
// two masks implement a complex conjugate quarter-turn without a multiply.
constexpr int max_slope = 51;
struct Rotation { std::uint8_t swap, invert_i; };
constexpr auto rotations() {
    std::array<std::array<Rotation,64>,2*max_slope+1> table{};
    for (int slope=-max_slope; slope<=max_slope; ++slope) {
        for (int phase=0; phase<64; ++phase) {
            unsigned swap=0, inv=0;
            for (int k=0; k<8; ++k) {
                const unsigned angle = static_cast<unsigned>(phase*16+slope*k+128);
                const unsigned quadrant = (angle>>8)&3;
                swap |= (quadrant&1)<<k;
                inv |= (quadrant>>1)<<k;
            }
            table[slope+max_slope][phase] = {static_cast<std::uint8_t>(swap),
                                           static_cast<std::uint8_t>(inv)};
        }
    }
    return table;
}
constexpr auto rotation = rotations();

inline int ones(std::uint32_t x) {
#if defined(__ARM_ARCH_8M_MAIN__)
    // Cortex-M33 has no scalar POPCNT. Keep the SWAR reduction inline.
    x -= (x>>1)&0x55555555u;
    x = (x&0x33333333u) + ((x>>2)&0x33333333u);
    x = (x+(x>>4))&0x0f0f0f0fu;
    return static_cast<int>((x*0x01010101u)>>24);
#else
    return __builtin_popcount(x);
#endif
}

struct Counts { int i{}, q{}; };
inline void add_mismatches(Counts& sum, std::uint32_t i, std::uint32_t q,
                           std::uint32_t replica, std::uint32_t valid) {
    sum.i += ones((i^replica)&valid);
    sum.q += ones((q^replica)&valid);
}
IQf signed_sum(Counts n) {
    return {float(int(half_chips)-2*n.i), float(int(half_chips)-2*n.q)};
}
}

Replica ca_replica(const Code& code) {
    Replica out;
    for (unsigned n=0; n<32*out.words.size(); ++n)
        out.words[n/32] |= std::uint32_t(code[(n%half_chips)/2]<0)<<(n%32);
    return out;
}

bool preprocess(const PackedBlock& block, Epoch& out) {
    if (!block.bytes || block.size!=bytes_per_ms) return false;
    out.first_sample=block.first_sample;
    out.discontinuity=block.discontinuity;
    const auto& a=adc[block.first_sample&3], &b=adc[(block.first_sample+2)&3];
    const std::uint8_t* in=block.bytes;
    for (unsigned w=0; w<words_per_epoch; ++w) {
        const unsigned count=std::min(32u,half_chips-32*w);
        std::uint32_t i=0, q=0;
        for (unsigned n=0; n<count; ++n, in+=4) {
            const unsigned xy = a[in[0]] + b[in[1]] + a[in[2]] + b[in[3]];
            i = (i>>1) | ((~xy&0x80u)<<24);
            q = (q>>1) | ((~xy&0x8000u)<<16);
        }
        out.i[w]=i>>(32-count);
        out.q[w]=q>>(32-count);
    }
    return true;
}

Sums correlate(const Epoch& epoch, const Replica& code, double cp, double fd, double turns) {
    if (!std::isfinite(cp) || !std::isfinite(fd) || !std::isfinite(turns) || std::abs(fd)>101000)
        return {};
    constexpr double one_turn = 4294967296.;
    const double carrier = std::remainder(turns,1.) + fd*(3.5/raw_hz);
    auto phase = static_cast<std::uint32_t>(std::llround(carrier*one_turn)) + (1u<<25);
    const auto step = static_cast<std::uint32_t>(std::llround(8*fd/baseband_hz*one_turn));
    const auto& rotate = rotation[max_slope + int(std::lround(fd/baseband_hz*1024))];

    // Track the first late half-chip index in Q16.16. Dither the aperture
    // within each half chip; early and late use exactly the same choice.
    const double phase0 = std::remainder(cp,double(code_length))*2
                        + (3.5/8 + 15.5*fd/l1_hz) - 1.5;
    int index = static_cast<int>(std::llround(phase0*frac))%period;
    if (index<0) index+=period;
    const int advance = static_cast<int>(std::lround(32*frac*(1+fd/l1_hz)));
    auto dither=std::uint32_t(epoch.first_sample/8)*0x9e3779b9u;
    Counts e{}, p{}, l{};
    for (unsigned w=0; w<words_per_epoch; ++w) {
        unsigned bit=unsigned(index + int(dither>>16))>>16;
        if (bit>=half_chips) bit-=half_chips;
        dither+=0x9e3779b9u;
        index+=advance;
        if (index>=period) index-=period;
        const unsigned j=bit/32, s=bit%32;
        const auto* ca=code.words.data()+j;
        const std::uint64_t window = (std::uint64_t(ca[1])<<32) | ca[0];
        const auto late=static_cast<std::uint32_t>(window>>s);
        const auto next=static_cast<std::uint32_t>(((std::uint64_t(ca[2])<<32)|ca[1])>>s);
        const auto prompt=(late>>1)|(next<<31);
        const auto early=(late>>2)|(next<<30);
        std::uint32_t swap=0, inv=0;
        for (unsigned k=0; k<32; k+=8) {
            const Rotation r=rotate[phase>>26];
            phase+=step;
            swap |= std::uint32_t(r.swap)<<k;
            inv |= std::uint32_t(r.invert_i)<<k;
        }
        const auto select = (epoch.i[w]^epoch.q[w])&swap;
        const auto i=(epoch.i[w]^select)^inv;
        const auto q=(epoch.q[w]^select)^(inv^swap);
        constexpr unsigned tail=half_chips%32;
        const std::uint32_t valid = w==words_per_epoch-1 ? (1u<<tail)-1 : ~0u;
        add_mismatches(l,i,q,late,valid);
        add_mismatches(p,i,q,prompt,valid);
        add_mismatches(e,i,q,early,valid);
    }
    return {signed_sum(e),signed_sum(p),signed_sum(l)};
}
}
