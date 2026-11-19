// SPDX-License-Identifier: Apache-2.0
#include "receiver/bitplane.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sig=eph::signal;
namespace bp=eph::signal::bitplane;
namespace {
constexpr double tau=6.2831853071795864769;
using Bytes=std::array<std::uint8_t,sig::bytes_per_ms>;
void check(bool ok,const char* msg) {
    if (!ok) {std::fprintf(stderr,"FAIL: %s\n",msg);std::exit(1);}
}
void synth(Bytes& raw,const sig::Code& code,double cp,double fd,unsigned start) {
    for (int n=0;n<sig::raw_per_ms;++n) {
        const double t=n/sig::raw_hz;
        const auto chip=unsigned(std::floor(cp+t*(sig::chip_hz+fd/1540.)))%1023;
        const double phase=tau*(fd*t+((n+start)&3)*.25+.15);
        auto quant=[](double v){return (v<0 ? 2 : 0) | (std::abs(v)>.60 ? 1 : 0);};
        const int b=4*quant(code[chip]*std::cos(phase))+quant(code[chip]*std::sin(phase));
        if (n&1) raw[n/2]|=b<<4; else raw[n/2]=b;
    }
}
bp::Correlations reference(const Bytes& raw,const sig::Code& code,double cp,double fd,
                           unsigned origin,bool frozen) {
    bp::Correlations r{};
    for (int n=0;n<sig::raw_per_ms;++n) {
        const int word=n/32,first=32*word,count=std::min(32,sig::raw_per_ms-first);
        const auto x=sig::decode_nibble(raw[n/2]>>(4*(n&1)));
        const double ct=(frozen ? first+(count-1)*0.5 : n)/sig::raw_hz;
        const double angle=tau*(ct*fd+((n+origin)&3)*0.25);
        const double i=x.i*std::cos(angle)+x.q*std::sin(angle),q=x.q*std::cos(angle)-x.i*std::sin(angle);
        const double phase=cp+n/16.+(frozen ? first : n)*fd/(1540.*sig::raw_hz);
        auto add=[&](sig::IQf& y,double shift) {
            const int chip=(int(std::floor(phase+shift))+1023)%1023;
            y.i+=float(code[chip]*i);y.q+=float(code[chip]*q);
        };
        add(r.prompt,0);add(r.early,.5);add(r.late,-.5);
    }
    return r;
}
float diff(sig::IQf a,sig::IQf b) {return std::hypot(a.i-b.i,a.q-b.q);}
}

int main(int argc,char** argv) {
    const bool bench=argc==2 && std::strcmp(argv[1],"--bench")==0;
    bp::Planes planes;
    bp::Prefixes prefixes;
    sig::Code code;
    sig::ca_code(23,code);
    Bytes bytes;
    for (double hz:{-70000.,-1250.,0.,10250.,70000.}) {
        for (unsigned skip:{0u,1u,2u,3u}) {
            constexpr double phase=1022.2732;
            synth(bytes,code,phase,hz,skip);
            check(bp::from_packed({skip,bytes.data(),bytes.size()},planes),"transpose capture");
            check(bp::from_packed({skip,bytes.data(),bytes.size()},prefixes),"build shared raw prefixes");
            for (unsigned first:{0u,1u,4u,15u,30u,61u,2046u,16335u}) {
                const unsigned count=std::min(unsigned(sig::raw_per_ms)-first,2053u);
                const double rate=(sig::chip_hz+hz/1540.)/sig::raw_hz;
                const auto a=bp::track_interval(planes,code,first,count,phase,rate,hz,.201);
                const auto b=bp::track_interval(prefixes,code,first,count,phase,rate,hz,.201);
                check(!diff(a.prompt,b.prompt) && !diff(a.early,b.early) && !diff(a.late,b.late) &&
                      a.energy==b.energy,"prefix and popcount agree for partial and unaligned code periods");
            }
            const auto y=bp::correlate(planes,code,phase,hz);
            const auto expected=reference(bytes,code,phase,hz,skip,true);
            check(diff(y.prompt,expected.prompt)<6 && diff(y.early,expected.early)<6 &&
                  diff(y.late,expected.late)<6,"bit-plane correlator matches sample-by-sample equivalent");
            const auto full=reference(bytes,code,phase,hz,skip,false);
            const double amp=std::hypot(full.prompt.i,full.prompt.q);
            const double gain=20*std::log10(std::hypot(y.prompt.i,y.prompt.q)/amp);
            check(std::abs(gain)<.40 && diff(y.prompt,full.prompt)/amp<.046,
                  "full-rate comparison bounds block carrier approximation");
            if (skip==0 && bench) std::printf("doppler_hz=%.0f signal_gain_db=%.4f\n",hz,gain);
        }
    }
    if (bench) {
        using clock=std::chrono::steady_clock;
        for (unsigned channels:{1u,4u,8u}) {
            std::array<sig::Code,8> codes{};
            for(unsigned j=0;j<channels;++j) sig::ca_code(1+j,codes[j]);
            double pack_us=0,kernel_us=0;
            float checksum=0;
            for (int epoch=0;epoch<500;++epoch) {
                const auto a=clock::now();
                bp::from_packed({0,bytes.data(),bytes.size()},planes);
                const auto b=clock::now();
                for(unsigned j=0;j<channels;++j)
                    checksum+=bp::correlate(planes,codes[j],epoch*.012+j,2700.*j).prompt.i;
                const auto c=clock::now();
                pack_us+=std::chrono::duration<double,std::micro>(b-a).count();
                kernel_us+=std::chrono::duration<double,std::micro>(c-b).count();
            }
            std::printf("native planes channels=%u packed_to_planes_us=%.3f correlation_us=%.3f checksum=%.3f\n",
                        channels,pack_us/500,kernel_us/500,checksum);
        }
    }
    std::puts("Word-parallel E/P/L correlation and carrier approximation passed");
}
