// SPDX-License-Identifier: Apache-2.0
#include "receiver/signal.hpp"
#include "receiver/bitplane.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

namespace s = eph::signal;
namespace {
constexpr double tau = 6.2831853071795864769;
void require(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; std::exit(1); }
}
double circular_error(double a, double b) { return std::abs(std::remainder(a-b, 1023.)); }

// Independently derive the PRNs from the *delay* column of IS-GPS-200N
// table 3-Ia, rather than using the production G2 selected taps.
s::Code reference_code(unsigned prn) {
    const int delays[] = {5,6,7,8,17,18,139,140,141,251,252,254,255,256,257,258,
                          469,470,471,472,473,474,509,512,513,514,515,516,859,860,861,862};
    std::array<int,10> a{}, b{};
    a.fill(1); b.fill(1);
    s::Code g1{},g2{},code{};
    for (int n=0; n<1023; ++n) {
        g1[n]=a[9]; g2[n]=b[9];
        const auto fa=a[2]^a[9], fb=b[1]^b[2]^b[5]^b[7]^b[8]^b[9];
        std::rotate(a.rbegin(),a.rbegin()+1,a.rend());
        std::rotate(b.rbegin(),b.rbegin()+1,b.rend());
        a[0]=fa; b[0]=fb;
    }
    for (int n=0;n<1023;++n)
        code[n]=(g1[n]^g2[(n+1023-delays[prn-1])%1023]) ? -1 : 1;
    return code;
}
using Packed = std::array<std::uint8_t,s::bytes_per_ms>;
int encode(double x) { return (x<0 ? 2 : 0) | (std::abs(x)>1.0 ? 1 : 0); }

struct Sat { unsigned prn; double phase, doppler, ramp, amplitude; };

// A continuous analytic source before IF and two-bit quantization. Raw sample
// positions remain continuous across every tracking block and data transition.
class Source {
public:
    explicit Source(const Sat& sat, double sigma = 0.7, unsigned seed = 1701) :
        sat_(sat), code_(reference_code(sat.prn)), sigma_(sigma), rng_(seed) {}
    s::Epoch epoch(std::uint64_t first, Packed* saved = nullptr) {
        Packed packed{};
        for (unsigned n=0;n<s::raw_per_ms;++n) {
            const double t=(first+n)/s::raw_hz;
            const double doppler_cycles=sat_.doppler*t + sat_.ramp*t*t/2;
            const double chips = sat_.phase + s::chip_hz*t + doppler_cycles/1540.;
            const int chip=int(std::floor(chips))%1023;
            const int nav=(static_cast<int>(std::floor(chips/(20*1023))) & 1) ? -1 : 1;
            const double angle=tau*(0.213+(first+n)%4*0.25+doppler_cycles);
            const double a = sat_.amplitude*nav*code_[chip];
            // Draw in an explicit order; C++ does not order the operands of +.
            const double ni=gaussian(), nq=gaussian();
            const int nibble=4*encode(a*std::cos(angle)+sigma_*ni)+
                               encode(a*std::sin(angle)+sigma_*nq);
            packed[n/2] |= nibble << (4*(n%2));
        }
        if (saved) *saved=packed;
        s::Epoch out;
        require(s::preprocess({first,packed.data(),packed.size()},out),"valid capture block");
        return out;
    }
private:
    Sat sat_;
    s::Code code_;
    double sigma_;
    std::mt19937 rng_{1701};
    bool spare_ready_{};
    double spare_{};
    double gaussian() {
        if (spare_ready_) {
            spare_ready_=false;
            return spare_;
        }
        // Polar Box-Muller with a specified MT19937-to-double mapping. This
        // avoids normal_distribution's implementation-dependent test sequence.
        auto uniform=[this] {
            const double low=rng_(), high=rng_();
            return 2.0*std::ldexp(low+std::ldexp(high,32),-64)-1.0;
        };
        double x,y,r2;
        do {
            x=uniform(); y=uniform(); r2=x*x+y*y;
        } while (r2>1.0 || r2==0.0);
        const double scale=std::sqrt(-2.0*std::log(r2)/r2);
        spare_=y*scale;
        spare_ready_=true;
        return x*scale;
    }
};

void format_tests() {
    constexpr int levels[]={1,3,-1,-3};
    for (unsigned n=0;n<16;++n) {
        const auto iq=s::decode_nibble(n);
        require(iq.i==levels[n/4] && iq.q==levels[n%4],"all ADC states");
    }
    s::Epoch epoch;
    Packed raw;
    std::mt19937 rng(51);
    for (auto& b:raw) b=static_cast<std::uint8_t>(rng());
    for (unsigned origin=0; origin<4; ++origin) {
        require(s::preprocess({origin,raw.data(),raw.size()},epoch),"preprocess");
        s::Epoch in_place;
        std::memcpy(in_place.samples.data(), raw.data(), raw.size());
        const auto* bytes = reinterpret_cast<const uint8_t*>(in_place.samples.data());
        require(s::preprocess({origin, bytes, raw.size()}, in_place) &&
                std::memcmp(in_place.samples.data(), epoch.samples.data(), raw.size()) == 0,
                "in-place acquisition snapshot");
        for (int k=0;k<s::samples_per_ms;++k) {
            int i=0,q=0;
            for (int m=0;m<4;++m) {
                const int j=4*k+m;
                const auto n=(raw[j/2]>>(4*(j%2)))&15;
                const int xi=levels[n/4], xq=levels[n%4];
                constexpr int c[]={1,0,-1,0}, sn[]={0,-1,0,1};
                const auto phase=(origin+m)%4;
                i+=xi*c[phase]-xq*sn[phase]; q+=xi*sn[phase]+xq*c[phase];
            }
            require(epoch.samples[k].i==i && epoch.samples[k].q==q,"scalar IF mixer agrees");
        }
    }
    require(!s::preprocess({0,raw.data(),raw.size()-1},epoch),"truncated block");
    require(!s::preprocess({0,nullptr,raw.size()},epoch),"null input");
    require(!s::preprocess({UINT64_MAX,raw.data(),raw.size()},epoch),"timestamp overflow");
    s::Code out;
    for (unsigned prn=1;prn<=32;++prn) {
        require(s::ca_code(prn,out) && out==reference_code(prn),"all reference PRNs");
    }
    require(!s::ca_code(0,out) && !s::ca_code(33,out),"invalid PRNs");
}

s::Detection search(s::Acquisition& acq, const s::Epoch& e, unsigned prn, float lo, float hi) {
    require(acq.start(e,prn,lo,hi),"start search");
    require(!acq.step(0),"search yields");
    while (!acq.step(1)) {}
    return acq.best();
}

void fft_oracle(s::Acquisition& acq) {
    // Direct periodic dot product checks the wrapped, zero-padded FFT at all
    // 2046 lags (including the seam) without using its implementation.
    Source source({4,1022.5,0,0,1.4});
    auto e=source.epoch(0);
    const auto code=reference_code(4);
    double sum=0, peak=-1;
    int delay=-1;
    for (int k=0;k<2046;++k) {
        int si=0,sq=0;
        for (int n=0;n<2046;++n) {
            const auto a=e.samples[2*n], b=e.samples[2*n+1];
            const int sign=code[((n+2046-k)%2046)/2];
            si+=sign*(a.i+b.i); sq+=sign*(a.q+b.q);
        }
        const double power=double(si)*si+double(sq)*sq;
        sum+=power;
        if (power>peak) {peak=power;delay=k;}
    }
    auto found=search(acq,e,4,0,0);
    require(circular_error(-delay*0.5,found.code_phase_chips)<1e-6,"FFT lag agrees with direct search");
    require(std::abs(found.metric-peak*2046/sum)<0.03,"FFT peak-to-mean agrees with direct search");
}

void tracking_tests(s::Acquisition& acq, Sat sat) {
    Source source(sat,0.6);
    auto e=source.epoch(0);
    auto d=search(acq,e,sat.prn,-70000,70000);
    require(d.detected && std::abs(d.doppler_hz-sat.doppler)<251,"LEO Doppler acquired without hint");
    require(circular_error(d.code_phase_chips,sat.phase)<0.6,"acquisition phase convention");
    s::Confirmation confirmation;
    confirmation.start(d);
    require(confirmation.add(e) && !confirmation.best().detected,"same dwell cannot confirm");
    confirmation.start(d);
    for (unsigned n=1;n<=4;++n)
        require(confirmation.add(source.epoch(n*s::raw_per_ms))==(n==4),"fresh confirmation dwell");
    d=confirmation.best();
    require(d.detected,"fresh confirmation");
    s::Tracker tr;
    require(tr.start(d),"start tracking");
    std::uint64_t previous_epoch=0;
    double previous_end=0;
    s::Tracker population,prefix;
    require(population.start(d) && prefix.start(d),"start word trackers");
    s::bitplane::Planes planes;
    s::bitplane::Prefixes prefixes;
    Packed raw;
    unsigned count=0,word_count=0;
    int stable_polarity=0,word_polarity=0;
    for (int ms=5;ms<108;++ms) {
        e=source.epoch(std::uint64_t(ms)*s::raw_per_ms,&raw);
        const s::PackedBlock packed{e.first_sample,raw.data(),raw.size()};
        require(s::bitplane::from_packed(packed,planes) && s::bitplane::from_packed(packed,prefixes),
                "prepare both word layouts");
        const auto t=tr.update(e);
        const auto w=population.update(planes), p=prefix.update(prefixes);
        require(w.active && p.active && w.locked==p.locked && w.count==p.count &&
                w.doppler_hz==p.doppler_hz,"word backends use the same carrier and period decisions");
        for (unsigned j=0;j<w.count;++j) {
            const auto& a=w.prompts[j]; const auto& b=p.prompts[j];
            require(a.i==b.i && a.q==b.q && a.start_sample==b.start_sample && a.end_sample==b.end_sample,
                    "code-run prefix sums reproduce popcount coherent prompts exactly");
            if (ms>30) {
                const double ts=a.start_sample/s::raw_hz,center=(a.start_sample+a.end_sample)/(2*s::raw_hz);
                const double edge=sat.phase+s::chip_hz*ts+(sat.doppler+0.5*sat.ramp*ts)*ts/1540.;
                const double code=sat.phase+s::chip_hz*center+(sat.doppler+0.5*sat.ramp*center)*center/1540.;
                const int bit=(int(code/(20*1023))&1) ? -1 : 1;
                if (!word_polarity) word_polarity=a.symbol*bit;
                require(a.symbol*word_polarity==bit && circular_error(edge,0)<0.23,
                        "complete word-backend code periods and navigation bit signs");
            }
            ++word_count;
        }
        if (ms>30) require(w.locked && std::abs(t.doppler_hz-w.doppler_hz)<12 &&
                           circular_error(t.code_phase_chips,w.code_phase_chips)<0.14,
                           "scalar and word trackers agree on independently synthesized IF");
        require(t.active,"channel survives data flips");
        if (ms>20) require(t.locked,"confident lock");
        for (unsigned k=0;k<t.count;++k) {
            const auto& p=t.prompts[k];
            if (count) require(p.epoch==previous_epoch+1 && std::abs(p.start_sample-previous_end)<0.001,
                               "unique contiguous C/A periods");
            require(p.end_sample>p.start_sample && p.end_sample<=double(e.first_sample+s::raw_per_ms)+2,
                    "plausible period boundaries");
            if (ms>30) {
                const double center=(p.start_sample+p.end_sample)/(2*s::raw_hz);
                const double chips=sat.phase+s::chip_hz*center+(sat.doppler*center+0.5*sat.ramp*center*center)/1540.;
                const int truth=(int(chips/(20*1023))&1) ? -1 : 1;
                if (!stable_polarity) stable_polarity=truth*p.symbol;
                require(p.symbol*stable_polarity==truth,"LNAV bit sign matches truth modulo Costas polarity");
                // Fractional epoch timing includes quantizer, DLL and discrete-sample bias.
                const double ts=p.start_sample/s::raw_hz;
                const double edge=sat.phase+s::chip_hz*ts+(sat.doppler*ts+0.5*sat.ramp*ts*ts)/1540.;
                require(circular_error(edge,0)<0.23,"actual code epoch fractional timing");
                require(std::abs(t.doppler_hz-(sat.doppler+sat.ramp*center))<25,"carrier tracks rate");
            }
            previous_epoch=p.epoch; previous_end=p.end_sample; ++count;
        }
    }
    require(count>=100 && word_count==count,"full code period counts");
    planes.discontinuity=prefixes.discontinuity=true;
    require(!population.update(planes).active && !prefix.update(prefixes).active,"word trackers invalidate a gap");
    require(population.start(d) && prefix.start(d),"word trackers reacquire from a fresh detection");
    for (unsigned n=0;n<7;++n) {
        const auto block=source.epoch((n+110ull)*s::raw_per_ms,&raw);
        const s::PackedBlock packed{block.first_sample,raw.data(),raw.size()};
        s::bitplane::from_packed(packed,planes); s::bitplane::from_packed(packed,prefixes);
        const auto w=population.update(planes),p=prefix.update(prefixes);
        if (n==6) require(w.locked && p.locked,"both word trackers lock again after a discontinuity");
    }
    auto gap=e;
    gap.first_sample+=s::raw_per_ms*2;
    require(!tr.update(gap).active,"unmarked sample gap invalidates tracker");
    require(tr.start(d),"restart");
    gap.discontinuity=true;
    require(!tr.update(gap).active,"explicit gap invalidates tracker");
    require(tr.start(d),"restart for dropout");
    s::Epoch silence{};
    silence.first_sample=d.first_sample;
    for (unsigned i=0;i<20;++i) {
        auto t=tr.update(silence);
        if (i==19) require(!t.active && !t.locked,"signal loss declared");
        silence.first_sample+=s::raw_per_ms;
    }
}

double p95(std::vector<double>& values) {
    std::sort(values.begin(),values.end());
    return values[static_cast<std::size_t>(std::ceil(0.95*values.size()))-1];
}

void sensitivity_run() {
    // Unit-variance independent Gaussian noise in I and Q. For complex tone
    // amplitude A before quantization, C/N0 = A^2 Fs /(2 sigma^2).
    // These are idealized baseband measurements, without the analog IF filter.
    s::Acquisition acq;
    for (double db:{38.,40.,45.}) {
        unsigned acquired=0, incorrect=0, false_hits=0;
        const double amplitude=std::sqrt(2*std::pow(10.,db/10)/s::raw_hz);
        for (unsigned run=0;run<64;++run) {
            Sat sat{1+run%32, std::fmod(43.2941*run,1023.), -51000+1631.1*run, 90, amplitude};
            Source src(sat,1.,3301+run);
            std::array<s::Epoch,8> epochs;
            for (unsigned n=0;n<8;++n) epochs[n]=src.epoch(std::uint64_t(n)*s::raw_per_ms);
            auto perform=[&](unsigned prn) {
                require(acq.start(epochs[0],prn),"multi-dwell init");
                for (int n=1;n<4;++n) require(acq.add_dwell(epochs[n]),"multi-dwell acquisition");
                while (!acq.step(16)) {}
                auto found=acq.best();
                s::Confirmation verifier;
                verifier.start(found);
                for (int n=4;n<8;++n) if (verifier.add(epochs[n])) break;
                return verifier.best();
            };
            auto confirmed=perform(sat.prn);
            if (confirmed.detected) {
                const double t=confirmed.first_sample/s::raw_hz;
                const double phase=sat.phase+(sat.doppler+0.5*t*sat.ramp)*t/1540.;
                if (std::abs(confirmed.doppler_hz-sat.doppler)<500 &&
                    circular_error(confirmed.code_phase_chips,phase)<0.75) ++acquired;
                else ++incorrect;
            }
            false_hits+=perform(sat.prn%32+1).detected;
        }
        std::cout<<"model_cn0_db_hz="<<db<<" trials=64 four_plus_four_acquired="<<acquired
                 <<" wrong_cells="<<incorrect<<" absent_prn_confirmed="<<false_hits<<'\n';
        require(incorrect==0 && false_hits==0,"confirmed wrong satellite/code cell");
        if (db>=45) require(acquired>=56,"strong signal sensitivity regression");
        if (db>=40) require(acquired>=30,"nominal acquisition sensitivity regression");
    }
    for (double fd:{-51157.,0.,22244.}) {
        Sat sat{3,50.375,fd,72,std::sqrt(2.0e4/s::raw_hz)};
        Source src(sat,1.0);
        s::Tracker scalar,word;
        const s::Detection seed{sat.prn,0,sat.phase,float(sat.doppler),40,true};
        require(scalar.start(seed) && word.start(seed),"tracking sweep seed");
        s::bitplane::Prefixes prefixes;
        Packed raw;
        std::array<std::vector<double>,2> frequency,code;
        for (unsigned n=0;n<1000;n++) {
            const auto e=src.epoch(std::uint64_t(n)*s::raw_per_ms,&raw);
            s::bitplane::from_packed({e.first_sample,raw.data(),raw.size()},prefixes);
            const std::array<s::TrackingResult,2> states{scalar.update(e),word.update(prefixes)};
            for (unsigned i=0;i<states.size();++i) {
                const auto& state=states[i];
                require(state.active,"40 dB-Hz tracking continuity");
                if (n<550) continue;
                frequency[i].push_back(std::abs(state.doppler_hz-(fd+sat.ramp*(n+1)/1000.)));
                for (unsigned j=0;j<state.count;++j) {
                    const double t=state.prompts[j].start_sample/s::raw_hz;
                    code[i].push_back(circular_error(sat.phase+s::chip_hz*t+(fd+0.5*sat.ramp*t)*t/1540.,0));
                }
            }
        }
        for (unsigned i=0;i<2;++i) {
            const auto hz=p95(frequency[i]),chips=p95(code[i]);
            std::cout<<"model_cn0_db_hz=40 kernel="<<(i ? "word" : "scalar")
                     <<" initial_doppler_hz="<<fd<<" rate_hz_per_s=72 p95_error_hz="
                     <<hz<<" p95_code_chips="<<chips<<'\n';
            require(hz<2.5 && chips<0.09,"weak-signal tracking observable regression");
        }
    }
}

void test_slipping_epochs(double fd,double initial_phase,unsigned expected_count) {
    Sat sat{1,initial_phase,fd,0,2};
    Source src(sat,0.1);
    s::Tracker channel;
    require(channel.start({sat.prn,0,sat.phase,float(fd),60,true}),"epoch test init");
    bool seen=false;
    for (unsigned n=0;n<150;++n) {
        auto t=channel.update(src.epoch(std::uint64_t(n)*s::raw_per_ms));
        if (n>2 && t.count==expected_count) seen=true;
        require(t.active,"epoch boundary test stays active");
    }
    require(seen,"zero/two complete code periods handled in one nominal DMA epoch");
}

void multiple_dwell_timing(s::Acquisition& acq) {
    Sat sat{7,1021.75,-65432,-40,1.3};
    Source src(sat,0.4);
    const auto first=src.epoch(0);
    require(acq.start(first,sat.prn,-66100,-64700),"multiple-dwell search");
    require(!acq.add_dwell(first),"overlapping dwell rejected");
    for (unsigned t:{9u,37u,91u}) require(acq.add_dwell(src.epoch(t*s::raw_per_ms)),"irregular dwell spacing");
    require(!acq.add_dwell(src.epoch(95*s::raw_per_ms)),"fifth dwell rejected");
    while (!acq.step(3)) {}
    auto d=acq.best();
    require(d.detected && circular_error(d.code_phase_chips,sat.phase)<0.6,
            "carrier-aided delay aligns cells across unequal dwell spacing");
    require(!acq.add_dwell(first),"cannot add after FFT search starts");
}

void benchmark() {
    using clock=std::chrono::steady_clock;
    Source src({1,0,0,0,1});
    auto e=src.epoch(0);
    s::Acquisition acq;
    s::Tracker t[8];
    // A shared input measures memory contention and the per-channel kernels, not RF sensitivity.
    for (unsigned channels:{1,4,8}) {
        for (unsigned j=0;j<channels;++j) require(t[j].start({1,0,0,0,50,true}),"bench init");
        e.first_sample=0;
        double worst_us=0;
        const auto begin=clock::now();
        for (int n=0;n<1000;++n) {
            auto before=clock::now();
            for (unsigned j=0;j<channels;++j) require(t[j].update(e).active,"bench tracking");
            worst_us=std::max(worst_us,std::chrono::duration<double,std::micro>(clock::now()-before).count());
            e.first_sample+=s::raw_per_ms;
        }
        auto mean_us=std::chrono::duration<double,std::micro>(clock::now()-begin).count()/1000;
        std::cout<<"native channels="<<channels<<" mean_us_per_ms="<<mean_us<<" max_us="<<worst_us<<'\n';
    }
    auto begin=clock::now();
    (void)search(acq,e,1,-70000,70000);
    std::cout<<"native full_doppler_one_prn_ms="<<std::chrono::duration<double,std::milli>(clock::now()-begin).count()
             <<" search_workspace_bytes="<<sizeof(acq)<<'\n';
}
} // namespace

int main(int argc,char** argv) {
    if (argc==2 && std::strcmp(argv[1],"--sweep")==0) { sensitivity_run();return 0; }
    if (argc==2 && std::strcmp(argv[1],"--bench")==0) { benchmark();return 0; }
    format_tests();
    test_slipping_epochs(+58000,1020.5,2);
    test_slipping_epochs(-58000,2.5,0);
    s::Acquisition acq;
    fft_oracle(acq);
    multiple_dwell_timing(acq);
    tracking_tests(acq,{3,888.375,48072,72,1.5});
    tracking_tests(acq,{22,71.6,-51157,-85,1.5});
    Source noise({1,0,0,0,0},0.8);
    auto e=noise.epoch(0);
    auto d=search(acq,e,1,-70000,70000);
    s::Confirmation verifier;
    verifier.start(d);
    for (int n=1;n<=4;++n) if (verifier.add(noise.epoch(n*s::raw_per_ms))) break;
    require(!verifier.best().detected,"noise fails fresh verification");
    require(!acq.start(e,0) && acq.step(),"bad PRN clears search");
    require(!acq.start(e,1,300,-300),"reversed Doppler range");
    std::cout<<"Signal format, direct oracle, search, tracking, LNAV polarity and dropout passed\n";
}
