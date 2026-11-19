// SPDX-License-Identifier: Apache-2.0
#include "coarse.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eph::signal::coarse {
namespace {
constexpr int frac = 1 << 16;
constexpr int period = half_chips * frac;
constexpr double baseband_hz = raw_hz / 8;

// Four chronological ADC nibbles make one complete Fs/4 turn. The two
// signed sums plus a bias of 64 fit in separate bytes. Combining two
// complete turns sets each byte's high bit iff that axis is nonnegative.
struct RawSums { std::uint16_t lut[65536]; };
struct Pair { int i,q; };
constexpr RawSums raw_sums() {
    constexpr int level[]={1,3,-1,-3};
    Pair pair[256]{};
    for(unsigned b=0;b<256;++b)
        pair[b]={level[(b>>2)&3]+level[(b>>4)&3], level[b&3]-level[(b>>6)&3]};
    RawSums out{};
    for(unsigned k=0;k<65536;++k) {
        const auto a=pair[k&255], b=pair[k>>8];
        out.lut[k]=std::uint16_t(64+a.i-b.i + 256*(64+a.q-b.q));
    }
    return out;
}
// Pico SDK copies .time_critical into SRAM on flash boots. This table sees
// noise-driven indices, so XIP cache locality cannot bound its access time.
#if defined(__ARM_ARCH_8M_MAIN__)
__attribute__((section(".time_critical.eph_coarse_lut")))
#endif
constexpr auto sums=raw_sums();

template<unsigned phase>
void pack_epoch(const std::uint8_t* src, Epoch& out) {
    for(unsigned w=0;w<words_per_epoch;++w) {
        std::uint32_t i=0,q=0;
        const unsigned count=std::min(32u,half_chips-32*w);
        for(unsigned n=0;n<count;n+=8) {
            unsigned bits=0;
            const unsigned used=std::min(8u,count-n);
            for(unsigned k=0;k<used;++k,src+=4) {
                std::uint16_t a,b;
                std::memcpy(&a,src,2); std::memcpy(&b,src+2,2);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
                a=__builtin_bswap16(a); b=__builtin_bswap16(b);
#endif
                const unsigned xy=sums.lut[a]+sums.lut[b];
                unsigned negative;
                if constexpr (phase==0) negative=~xy;
                if constexpr (phase==1) negative=((~xy>>8)&255u)|((xy-0x0101u)<<8);
                if constexpr (phase==2) negative=xy-0x0101u;
                if constexpr (phase==3) negative=((xy-0x0101u)>>8)|(~xy<<8);
                bits=(bits>>1)|(negative&0x8080u);
            }
            // Eight iterations pack both axes into independent byte lanes.
            bits>>=8-used;
            i|=(bits&255u)<<n; q|=((bits>>8)&255u)<<n;
        }
        out.i[w]=i; out.q[w]=q;
    }
}
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

// Sample one sine/cosine cycle by complex recurrence at compile time. Linear
// interpolation across a 1/256-turn interval keeps amplitude error below 8e-5.
constexpr auto oscillator_table() {
    std::array<IQf,257> table{};
    double c=1,s=0;
    constexpr double dc=.9996988186962042201, ds=.024541228522912288;
    for(unsigned n=0;n<256;++n) {
        table[n]={float(c),float(s)};
        const double next=c*dc-s*ds;
        s=s*dc+c*ds; c=next;
    }
    table[256]=table[0];
    return table;
}
constexpr auto oscillator=oscillator_table();
IQf phasor(std::uint32_t phase) {
    const unsigned j=phase>>24;
    const float f=float((phase>>8)&65535u)*(1.f/65536);
    const auto a=oscillator[j], b=oscillator[j+1];
    return {a.i+f*(b.i-a.i), a.q+f*(b.q-a.q)};
}

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
IQf signed_sum(Counts n,unsigned count) {
    return {float(int(count)-2*n.i), float(int(count)-2*n.q)};
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
    switch (block.first_sample&3) {
        case 0: pack_epoch<0>(block.bytes,out); break;
        case 1: pack_epoch<1>(block.bytes,out); break;
        case 2: pack_epoch<2>(block.bytes,out); break;
        default: pack_epoch<3>(block.bytes,out); break;
    }
    return true;
}

template<bool smooth_carrier>
static Sums interval(const Epoch& epoch, const Replica& code, unsigned first, unsigned count,
                    double cp, double chips_per_half, double fd, double turns) {
    if (!count || first>=half_chips || count>half_chips-first ||
        !std::isfinite(cp) || !std::isfinite(turns) || !std::isfinite(fd) ||
        std::abs(fd)>101000 || !(chips_per_half>=.49 && chips_per_half<=.51))
        return {};
    const unsigned skip=first%32, end=first+count;
    constexpr double one_turn = 4294967296.;
    const double input_turns=std::abs(turns)<=1.1 ? turns : std::remainder(turns,1.);
    const double carrier=input_turns-fd*skip/baseband_hz;
    const auto first_phase=static_cast<std::uint32_t>(std::llround(input_turns*one_turn));
    auto phase = static_cast<std::uint32_t>(std::llround(carrier*one_turn)) + (1u<<25);
    const auto step = static_cast<std::uint32_t>(std::llround(8*fd/baseband_hz*one_turn));
    const auto half_step=static_cast<std::uint32_t>(std::llround(fd*(one_turn/(2*baseband_hz))));
    const auto& rotate = rotation[max_slope + int(std::lround(fd/baseband_hz*1024))];

    // Track the first late half-chip index in Q16.16. Dither the aperture
    // within each half chip; early and late use exactly the same choice.
    const double input_code=std::abs(cp)<=2.*code_length ? cp : std::remainder(cp,double(code_length));
    const double phase0 = (input_code-skip*chips_per_half)*2
                        + 15.5*(2*chips_per_half-1) - 1.5;
    int index = static_cast<int>(std::llround(phase0*frac))%period;
    if (index<0) index+=period;
    const int advance = static_cast<int>(std::lround(32*frac*(2*chips_per_half)));
    auto dither=(std::uint32_t(epoch.first_sample/8)+first/32)*0x9e3779b9u;
    Counts e{}, p{}, l{};
    Sums output{};
    // Accumulate integer E/P/L across complete words until 1/32 turn
    // elapses. With <= 31.25 Hz over 1 ms, one rotation covers the epoch.
    unsigned bucket_start=first, bucket_words=words_per_epoch, close_word=0;
    if constexpr (smooth_carrier) {
        if (std::abs(fd)>baseband_hz/(32.*count)) {
            const auto n=static_cast<unsigned>(baseband_hz/(32.*std::abs(fd)))+1;
            bucket_words=std::max(1u,n/32);
        }
        close_word=first/32+bucket_words;
    }
    for (unsigned w=first/32; 32*w<end; ++w) {
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
        const auto lo=w==first/32 ? skip : 0;
        const auto hi=std::min(32u,end-32*w);
        const auto valid=(~0u<<lo)&(~0u>>(32-hi));
        if constexpr (smooth_carrier) {
            const auto i=epoch.i[w], q=epoch.q[w];
            add_mismatches(e,i,q,early,valid);
            add_mismatches(p,i,q,prompt,valid);
            add_mismatches(l,i,q,late,valid);
            const unsigned stop=32*w+hi;
            if (w+1==close_word || stop==end) {
                const unsigned n=stop-bucket_start;
                const unsigned half_samples=(bucket_start-first)+(stop-first)-1;
                const auto lo=phasor(first_phase+half_step*half_samples);
                auto rotate_sum=[&](IQf& out,Counts v) {
                    const auto x=signed_sum(v,n);
                    out.i+=lo.i*x.i+lo.q*x.q; out.q+=lo.i*x.q-lo.q*x.i;
                };
                rotate_sum(output.early,e); rotate_sum(output.prompt,p); rotate_sum(output.late,l);
                e={}; p={}; l={}; bucket_start=stop; close_word+=bucket_words;
            }
            continue;
        }
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
        add_mismatches(l,i,q,late,valid);
        add_mismatches(p,i,q,prompt,valid);
        add_mismatches(e,i,q,early,valid);
    }
    if constexpr (smooth_carrier) return output;
    return {signed_sum(e,count),signed_sum(p,count),signed_sum(l,count)};
}

Sums track_interval(const Epoch& epoch,const Replica& code,unsigned first,unsigned count,
                    double cp,double chips_per_half,double fd,double turns) {
    if (std::abs(fd)<=2000.)
        return interval<true>(epoch,code,first,count,cp,chips_per_half,fd,turns);
    return interval<false>(epoch,code,first,count,cp,chips_per_half,fd,turns);
}

Sums correlate(const Epoch& epoch,const Replica& code,double cp,double fd,double turns) {
    const double rate=.5*(1+fd/l1_hz);
    return interval<false>(epoch,code,0,half_chips,cp+3.5*rate/8,rate,fd,turns+3.5*fd/raw_hz);
}
}
