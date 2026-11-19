// SPDX-License-Identifier: Apache-2.0
#include "receiver/coarse.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>

namespace sig = eph::signal;
namespace low = sig::coarse;
namespace {
using Bytes = std::array<std::uint8_t,sig::bytes_per_ms>;
using Z = std::complex<double>;
constexpr double tau=6.28318530717958647692529;
double amplitude(sig::IQf v) {return std::hypot(v.i,v.q);}
int sign(unsigned plane,unsigned k) {return ((plane>>k)&1)?-1:1;}

void packing() {
    std::mt19937 random(793);
    Bytes bytes{};
    sig::Code code{};
    sig::ca_code(17,code);
    const auto replica=low::ca_replica(code);
    for(unsigned bit=0;bit<32*replica.words.size();++bit)
        assert(sign(replica.words[bit/32],bit%32)==code[(bit/2)%1023]);
    for(unsigned origin=0;origin<4;++origin) {
        for(auto& b:bytes) b=static_cast<std::uint8_t>(random());
        low::Epoch out;
        assert(!low::preprocess({0,nullptr,0},out));
        assert(low::preprocess({origin,bytes.data(),bytes.size(),true},out));
        assert(out.first_sample==origin && out.discontinuity);
        alignas(4) std::array<std::uint8_t, sig::bytes_per_ms + 3> shifted{};
        for (unsigned offset = 1; offset <= 3; ++offset) {
            std::memcpy(shifted.data() + offset, bytes.data(), bytes.size());
            low::Epoch unaligned;
            assert(low::preprocess({origin, shifted.data() + offset, bytes.size(), true}, unaligned));
            assert(unaligned.i == out.i && unaligned.q == out.q);
        }
        for(unsigned k=0;k<low::half_chips;++k) {
            int i=0,q=0;
            for(unsigned j=0;j<8;++j) {
                const unsigned n=8*k+j;
                const auto raw=sig::decode_nibble(bytes[n/2]>>(4*(n&1)));
                switch ((n+origin)&3) {
                    case 0: i+=raw.i; q+=raw.q; break;
                    case 1: i+=raw.q; q-=raw.i; break;
                    case 2: i-=raw.i; q-=raw.q; break;
                    default:i-=raw.q; q+=raw.i; break;
                }
            }
            assert(sign(out.i[k/32],k%32)==(i<0?-1:1));
            assert(sign(out.q[k/32],k%32)==(q<0?-1:1));
        }
        assert(out.i.back()>>30==0 && out.q.back()>>30==0);
        constexpr double chip=.397, fd=13375., turns=.313;
        const double rate=.5*(1+fd/sig::l1_hz);
        const double center_chip=chip+3.5*rate/8, center_turn=turns+fd*3.5/sig::raw_hz;
        const auto all=low::correlate(out,replica,chip,fd,turns);
        for(unsigned split:{1u,7u,32u,871u,1000u,2045u}) {
            const auto a=low::track_interval(out,replica,0,split,center_chip,rate,fd,center_turn);
            const auto b=low::track_interval(out,replica,split,low::half_chips-split,
                       center_chip+split*rate,rate,fd,center_turn+split*fd/2046000.);
            auto joins=[](sig::IQf a,sig::IQf b,sig::IQf full) {
                return a.i+b.i==full.i && a.q+b.q==full.q;
            };
            assert(joins(a.early,b.early,all.early) && joins(a.prompt,b.prompt,all.prompt)
                   && joins(a.late,b.late,all.late));
        }
        const auto bad=low::correlate(out,replica,0,NAN);
        assert(amplitude(bad.prompt)==0);
    }
}

struct Model {
    sig::Code code, wrong;
    low::Replica local, other;
    std::mt19937 random{7129};
    std::normal_distribution<double> noise{0,1};
    Model() {sig::ca_code(15,code); sig::ca_code(14,wrong);
             local=low::ca_replica(code); other=low::ca_replica(wrong);}

    // A full-rate, continuously rotating, undithered scalar oracle integrates
    // exactly the transmitted ADC samples. Independent component AWGN is
    // pre-quantization, threshold 1 sigma, with a 2-bit sign/magnitude ADC.
    Z generate(Bytes& bytes,unsigned origin,double cp,double fd,double phase,double a) {
        Z sum{};
        const double start=tau*(phase+(origin&3)*.25);
        Z carrier=std::polar(1.,start);
        const Z step=std::polar(1.,tau*(.25+fd/sig::raw_hz));
        auto quant=[](double x){return int(x<0)*2+int(std::abs(x)>=1.);};
        for(unsigned n=0;n<sig::raw_per_ms;++n) {
            const double chips=cp+n*(sig::chip_hz+fd/1540.)/sig::raw_hz;
            const int c=code[unsigned(std::floor(chips))%1023];
            const Z v=a*c*carrier+Z(noise(random),noise(random));
            const int b=4*quant(v.real())+quant(v.imag());
            if(n&1) bytes[n/2]|=std::uint8_t(b<<4); else bytes[n/2]=std::uint8_t(b);
            const auto raw=sig::decode_nibble(b);
            sum+=std::conj(carrier)*double(c)*Z(raw.i,raw.q);
            carrier*=step;
        }
        return sum;
    }
};
void detection(bool report) {
    Model model;
    const double a=std::sqrt(2*std::pow(10.,47./10)/sig::raw_hz);
    double scattered=0, scalar_noise=0, signal_mean=0, scalar_mean=0;
    constexpr int epochs=24;
    for(double fd:{-70000.,-3700.,0.,33500.,70000.}) {
        for(double cp:{1022.01,1022.267,1022.72}) {
            double signal=0, off=0, scalar=0, noise=0, reference_noise=0, side=0;
            for(int e=0;e<epochs;++e) {
                const unsigned origin=e*sig::raw_per_ms+(e%4);
                const double phase=.019+e*.037;
                Bytes bytes;
                low::Epoch data;
                const auto truth=model.generate(bytes,origin,cp,fd,phase,a);
                assert(low::preprocess({origin,bytes.data(),bytes.size()},data));
                auto actual=low::correlate(data,model.local,cp,fd,phase);
                signal+=actual.prompt.i;
                scalar+=truth.real();
                off+=std::pow(amplitude(low::correlate(data,model.other,cp,fd,phase).prompt),2);
                side+=(actual.early.i+actual.late.i)*.5;
                // Test the DLL discriminator direction either side of the peak.
                if(e==0) {
                    const auto earlier=low::correlate(data,model.local,cp-.5,fd,phase);
                    const auto later=low::correlate(data,model.local,cp+.5,fd,phase);
                    assert(amplitude(earlier.early)>amplitude(earlier.late));
                    assert(amplitude(later.early)<amplitude(later.late));
                }
                auto silence=model.generate(bytes,origin,cp,fd,phase,0);
                low::preprocess({origin,bytes.data(),bytes.size()},data);
                noise+=std::pow(amplitude(low::correlate(data,model.local,cp,fd,phase).prompt),2);
                reference_noise+=std::norm(silence);
            }
            assert(signal>side && signal*signal > epochs*off*5);
            signal_mean+=signal*signal;
            scalar_mean+=scalar*scalar;
            scattered+=noise;
            scalar_noise+=reference_noise;
            if(report) std::printf("doppler_hz=%.0f phase=%.3f signal_vs_wrong_prn_db=%.2f\n",
                                   fd,cp,10*std::log10(signal*signal/(epochs*off)));
        }
    }
    const double loss_db=10*std::log10((signal_mean/scattered)/(scalar_mean/scalar_noise));
    if(report) std::printf("47_dbhz_coherent_snr_change_db=%.3f\n",loss_db);
    assert(loss_db > -7 && loss_db < -.5);
}
#ifdef EPHEMERIS_COARSE_TRACKING
void closed_loop(double fd) {
    Model model;
    constexpr double initial=1022.72;
    const double rate=sig::chip_hz+fd/1540.;
    sig::Tracker tracker;
    assert(tracker.start({15,0,initial-.10,float(fd-80),20,true}));
    low::Epoch data;
    unsigned locked=0, prompts=0;
    double worst_stable_error=0;
    double last_time=0, final_frequency=0;
    for(unsigned ms=0;ms<560;++ms) {
        const auto origin=std::uint64_t(ms)*sig::raw_per_ms;
        const double cp=std::fmod(initial+ms*.001*rate,1023.);
        const double turns=std::remainder(.183+fd*ms*.001,1.);
        Bytes raw;
        const double amplitude=(ms/20)%2 ? -.28 : .28;
        model.generate(raw,static_cast<unsigned>(origin),cp,fd,turns,amplitude);
        assert(low::preprocess({origin,raw.data(),raw.size()},data));
        const auto out=tracker.update(data);
        assert(out.active);
        locked+=out.locked;
        final_frequency=out.doppler_hz;
        if (ms>=485) worst_stable_error=std::max(worst_stable_error,std::abs(fd-final_frequency));
        for(unsigned k=0;k<out.count;++k) {
            const auto& p=out.prompts[k];
            assert(p.start_sample>=last_time && p.end_sample>p.start_sample);
            last_time=p.end_sample;
            if(ms>50) {
                const double truth=(initial+last_time*rate/sig::raw_hz);
                assert(std::abs(std::remainder(truth,1023.))<.20);
            }
            ++prompts;
        }
    }
    assert(locked>545 && prompts>550 && std::abs(final_frequency-fd)<3.);
    if (std::abs(fd)<10) assert(worst_stable_error<1.);
    data.first_sample+=sig::raw_per_ms;
    data.discontinuity=true;
    assert(!tracker.update(data).active);
}
#endif
void benchmark() {
    Model m;
    Bytes bytes;
    m.generate(bytes,0,13.25,23000.,.123,.12);
    low::Epoch data;
    constexpr int runs=1000;
    using clock=std::chrono::steady_clock;
    double prep=0,corr=0,checksum=0;
    for(int k=0;k<runs;++k) {
        auto t=clock::now();
        low::preprocess({std::uint64_t(k)*sig::raw_per_ms,bytes.data(),bytes.size()},data);
        auto s=clock::now();
        for(int j=0;j<8;++j) checksum+=low::correlate(data,m.local,13.25+j/8.,23000.+j,.123).prompt.i;
        auto u=clock::now();
        prep+=std::chrono::duration<double,std::micro>(s-t).count();
        corr+=std::chrono::duration<double,std::micro>(u-s).count();
    }
    std::printf("native prep_us=%.3f per_channel_us=%.3f checksum=%.0f\n",prep/runs,corr/(runs*8),checksum);
}
}
int main(int argc,char** argv) {
    const bool bench=argc==2 && std::strcmp(argv[1],"--bench")==0;
    packing(); detection(bench);
#ifdef EPHEMERIS_COARSE_TRACKING
    closed_loop(-41234.);
    closed_loop(-4.583);
#endif
    if(bench) benchmark();
    std::puts("Half-chip correlation, ADC ordering, Doppler, DLL direction and SNR passed");
}
