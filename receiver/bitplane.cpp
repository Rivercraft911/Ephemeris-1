// SPDX-License-Identifier: Apache-2.0
#include "bitplane.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace eph::signal::bitplane {
namespace {
constexpr std::uint64_t one_chip = 1ull<<32;
constexpr std::uint64_t period = code_length*one_chip;
constexpr double tau = 6.2831853071795864769;
constexpr auto spread_table() {
    // Expand two successive bits from the same GPIO within one packed byte.
    std::array<std::array<std::uint8_t,4>,256> out{};
    for (unsigned b=0;b<256;++b)
        for (unsigned pin=0;pin<4;++pin)
            out[b][pin]=((b>>pin)&1)|(((b>>(pin+4))&1)<<1);
    return out;
}
constexpr auto spread=spread_table();
unsigned pop(std::uint32_t v) { return static_cast<unsigned>(__builtin_popcount(v)); }
std::uint32_t low_bits(unsigned count) {
    return count==32 ? ~0u : (1u<<count)-1;
}
int weighted_sum(std::uint32_t sign, std::uint32_t mag, int sum_magnitudes, std::uint32_t code_mask) {
    const auto invert=sign^code_mask;
    return sum_magnitudes-2*static_cast<int>(pop(invert)+2*pop(invert&mag));
}
}

Word from_pio(std::uint32_t qmag,std::uint32_t qsign,std::uint32_t imag,
              std::uint32_t isign,std::uint64_t first,unsigned count) {
    constexpr std::uint32_t iinvert[4]={0xcccccccc,0x66666666,0x33333333,0x99999999};
    constexpr std::uint32_t qinvert[4]={0x66666666,0x33333333,0x99999999,0xcccccccc};
    const unsigned rotation=first%4;
    const auto odd=(rotation&1)? 0x55555555u : 0xaaaaaaaau;
    count=std::min(count,32u);
    const auto valid=low_bits(count);
    Word out;
    // exp(-j*pi*n/2) is just an axis permutation and sign XOR at full precision.
    out.isign=(((isign&~odd)|(qsign&odd))^iinvert[rotation])&valid;
    out.qsign=(((qsign&~odd)|(isign&odd))^qinvert[rotation])&valid;
    out.imag=((imag&~odd)|(qmag&odd))&valid;
    out.qmag=((qmag&~odd)|(imag&odd))&valid;
    out.iweight=static_cast<int>(count+2*pop(out.imag));
    out.qweight=static_cast<int>(count+2*pop(out.qmag));
    return out;
}

bool from_packed(const PackedBlock& block,Planes& out) {
    if (!block.bytes || block.size!=bytes_per_ms) return false;
    out.first_sample=block.first_sample;
    out.discontinuity=block.discontinuity;
    for (unsigned w=0;w<words_per_ms;++w) {
        const auto count=std::min(32u,raw_per_ms-32*w);
        std::uint32_t lanes[4]{};
        for (unsigned b=0;b<count/2;++b) {
            const auto pair=spread[block.bytes[16*w+b]];
            for (unsigned pin=0;pin<4;++pin) lanes[pin]|=std::uint32_t(pair[pin])<<(2*b);
        }
        out.words[w]=from_pio(lanes[0],lanes[1],lanes[2],lanes[3],block.first_sample+32*w,count);
    }
    return true;
}

namespace {
struct Pair { std::int16_t first, both; std::uint8_t energy; };
constexpr auto make_pairs() {
    std::array<std::array<Pair,256>,4> out{};
    constexpr int levels[]={1,3,-1,-3};
    for (unsigned phase=0;phase<4;++phase) {
        for (unsigned b=0;b<256;++b) {
            int iq[2]{},energy=0;
            for (unsigned k=0;k<2;++k) {
                const unsigned v=b>>(4*k);
                const int i=levels[(v>>2)&3],q=levels[v&3];
                const int rotated[]={i+256*q, q-256*i, -i-256*q, -q+256*i};
                iq[k]=rotated[(phase+k)&3]; energy+=i*i+q*q;
            }
            out[phase][b]={static_cast<std::int16_t>(iq[0]),static_cast<std::int16_t>(iq[0]+iq[1]),
                           static_cast<std::uint8_t>(energy)};
        }
    }
    return out;
}
constexpr auto pairs=make_pairs();
}
bool from_packed(const PackedBlock& block,Prefixes& out) {
    if (!block.bytes || block.size!=bytes_per_ms) return false;
    out.first_sample=block.first_sample; out.discontinuity=block.discontinuity;
    const auto& a=pairs[block.first_sample&3], &b=pairs[(block.first_sample+2)&3];
    for (unsigned w=0;w<words_per_ms;++w) {
        auto& x=out.words[w];
        const unsigned count=std::min(32u,raw_per_ms-32*w);
        int sum=0,energy=0;
        for (unsigned n=0;n<count;n+=4) {
            const auto ab=a[block.bytes[16*w+n/2]], cd=b[block.bytes[16*w+n/2+1]];
            x.iq[n]=static_cast<std::int16_t>(sum+ab.first);
            sum+=ab.both; x.iq[n+1]=static_cast<std::int16_t>(sum);
            x.iq[n+2]=static_cast<std::int16_t>(sum+cd.first);
            sum+=cd.both; x.iq[n+3]=static_cast<std::int16_t>(sum);
            energy+=ab.energy+cd.energy;
        }
        x.energy=energy;
    }
    return true;
}

namespace {
Word slice(Word x, unsigned skip, unsigned count) {
    const auto mask=low_bits(count);
    x.isign=(x.isign>>skip)&mask; x.imag=(x.imag>>skip)&mask;
    x.qsign=(x.qsign>>skip)&mask; x.qmag=(x.qmag>>skip)&mask;
    x.iweight=static_cast<int>(count+2*pop(x.imag));
    x.qweight=static_cast<int>(count+2*pop(x.qmag));
    return x;
}

struct Runs { int a,b,c; };
Runs runs(const PrefixWord& word,unsigned skip,unsigned used,int base,int total,unsigned offset) {
    const unsigned edge1=std::min(used,16-(offset&15)), edge2=std::min(used,edge1+16);
    const int a=word.iq[skip+edge1-1]-base, ab=word.iq[skip+edge2-1]-base;
    return {a,ab-a,total-ab};
}
int despread(Runs r,const std::int8_t* signs) {
    return r.a*signs[0]+r.b*signs[1]+r.c*signs[2];
}
void accumulate(IQf& result,int iq,float c,float sn) {
    const int i=static_cast<int>((static_cast<unsigned>(iq)+128)&255)-128;
    const int q=(iq-i)/256;
    result.i+=c*i+sn*q; result.q+=c*q-sn*i;
}
int partial_energy(const PrefixWord& word,unsigned skip,unsigned used,int base) {
    int energy=0;
    for (unsigned n=0;n<used;++n) {
        const int sum=word.iq[skip+n],iq=sum-base;
        const int i=static_cast<int>((static_cast<unsigned>(iq)+128)&255)-128,q=(iq-i)/256;
        energy+=i*i+q*q; base=sum;
    }
    return energy;
}

template<bool interpolate,class Input>
Correlations integrate(const Input& data,const Code& code,unsigned first,unsigned count,
                       double phase_chips,double chips_per_sample,double doppler_hz,double carrier_turns) {
    Correlations result{};
    if (!count || first>=raw_per_ms || count>raw_per_ms-first || !std::isfinite(phase_chips) ||
        !std::isfinite(doppler_hz) || std::abs(doppler_hz)>101000 || !std::isfinite(carrier_turns) ||
        !(chips_per_sample>0 && chips_per_sample<1)) return result;
    // A four-chip 64-bit window holds all three replicas for 32 raw samples.
    // Freeze the code-rate residual within a word, then advance the actual NCO.
    double start=std::fmod(phase_chips-0.5,double(code_length));
    if (start<0) start+=code_length;
    std::uint64_t phase=static_cast<std::uint64_t>(start*one_chip);
    const auto step=static_cast<std::uint64_t>(std::llround(32*chips_per_sample*one_chip));
    const double radians=tau*doppler_hz/raw_hz;
    const float step_c=static_cast<float>(std::cos(32*radians));
    const float step_s=static_cast<float>(std::sin(32*radians));
    const unsigned end=first+count;
    float c=0,sn=0;
    // Fractional-replica dithering across words removes the early/late
    // discriminator's raw-sample staircase without adding more popcounts.
    std::uint32_t dither=static_cast<std::uint32_t>((data.first_sample+first)/32)*0x9e3779b9u;
    bool new_center=true;
    for (unsigned n=first;n<end;) {
        const unsigned skip=n%32;
        const unsigned used=std::min(32-skip,end-n);
        if (new_center || used!=32) {
            const double angle=tau*carrier_turns+(n-first+0.5*(used-1))*radians;
            c=static_cast<float>(std::cos(angle)); sn=static_cast<float>(std::sin(angle));
        }
        const auto& whole=data.words[n/32];
        // Center the dithered early/late replica on the requested chip phase.
        // At a raw-sample boundary the two neighboring masks have equal weight.
        std::uint64_t el_phase=phase;
        if constexpr (interpolate) {
            const auto sampled=phase+(dither>>4)+(std::uint64_t{1}<<27);
            el_phase=sampled>=(std::uint64_t{1}<<28) ? sampled-(std::uint64_t{1}<<28)
                                                     : sampled+period-(std::uint64_t{1}<<28);
            if (el_phase>=period) el_phase-=period;
            dither+=0x9e3779b9u;
        }
        unsigned chip=static_cast<unsigned>(el_phase>>32);
        const unsigned offset=static_cast<unsigned>(el_phase>>28)&15u;
        // The prompt is eight raw samples from late. Its undithered position
        // is within one sample of the selected E/L phase, even at a code seam.
        int prompt_offset=static_cast<int>(offset)+8;
        if constexpr (interpolate) {
            const auto a=static_cast<unsigned>(phase>>28), b=static_cast<unsigned>(el_phase>>28);
            if (a!=b) prompt_offset+=(a==b+1 || (a==0 && b==16*code_length-1)) ? 1 : -1;
        }
        if constexpr (std::is_same_v<Input,Prefixes>) {
            // Three contiguous chip runs cover a word. Early and late use
            // identical edge offsets with neighboring signs; prompt uses the
            // other half-chip offset. Each prefix lookup contains both axes.
            std::int8_t seam[4];
            const std::int8_t* signs=&code[chip];
            if (chip+3>=code_length) {
                for (auto& v:seam) {v=code[chip]; if (++chip==code_length) chip=0;}
                signs=seam;
            }
            const int base=skip ? whole.iq[skip-1] : 0, total=whole.iq[skip+used-1]-base;
            const auto el=runs(whole,skip,used,base,total,offset);
            const auto p=runs(whole,skip,used,base,total,static_cast<unsigned>(prompt_offset));
            accumulate(result.late,despread(el,signs),c,sn);
            accumulate(result.prompt,despread(p,signs+(prompt_offset>>4)),c,sn);
            accumulate(result.early,despread(el,signs+1),c,sn);
            result.energy+=used==32 ? whole.energy : partial_energy(whole,skip,used,base);
        } else {
            const auto x=used==32 ? whole : slice(whole,skip,used);
            result.energy+=4*(x.iweight+x.qweight)-6*static_cast<int>(used);
            std::uint64_t bits=0;
            for (unsigned k=0;k<4;++k) {
                if (code[chip]<0) bits|=std::uint64_t{0xffff}<<(16*k);
                if (++chip==code_length) chip=0;
            }
            auto add=[&](IQf& value,unsigned shift) {
                const auto mask=std::uint32_t(bits>>shift)&low_bits(used);
                const int i=weighted_sum(x.isign,x.imag,x.iweight,mask);
                const int q=weighted_sum(x.qsign,x.qmag,x.qweight,mask);
                value.i+=c*i+sn*q; value.q+=c*q-sn*i;
            };
            add(result.late,offset); add(result.prompt,static_cast<unsigned>(prompt_offset));
            add(result.early,offset+16);
        }
        const float nc=c*step_c-sn*step_s;
        sn=c*step_s+sn*step_c; c=nc;
        phase+=used==32 ? step : (step*used+16)/32;
        if (phase>=period) phase-=period;
        new_center=used!=32;
        n+=used;
    }
    return result;
}
}

Correlations track_interval(const Planes& data,const Code& code,unsigned first,unsigned count,
                            double phase_chips,double chips_per_sample,double doppler_hz,double carrier_turns) {
    return integrate<true>(data,code,first,count,phase_chips,chips_per_sample,doppler_hz,carrier_turns);
}
Correlations track_interval(const Prefixes& data,const Code& code,unsigned first,unsigned count,
                            double phase_chips,double chips_per_sample,double doppler_hz,double carrier_turns) {
    return integrate<true>(data,code,first,count,phase_chips,chips_per_sample,doppler_hz,carrier_turns);
}
Correlations correlate(const Planes& data,const Code& code,double phase_chips,double doppler_hz) {
    return integrate<false>(data,code,0,raw_per_ms,phase_chips,(chip_hz+doppler_hz/1540.)/raw_hz,doppler_hz,0);
}
} // namespace eph::signal::bitplane
