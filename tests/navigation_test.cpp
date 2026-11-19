// SPDX-License-Identifier: Apache-2.0
#include "receiver/navigation.hpp"
#include "fixtures/rollover.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace eph::nav;
namespace {
// RINEX 3.04 Appendix A, Table A7, G06 at 1999-09-02 17:51:44 GPST.
// Independent wire encoding from GPS-SDR-SIM 28ca29a, eph2sbf and
// generateNavMsg(week=1025,tow=409920). Includes preceding SF5. The second
// dataset advances IODC and IODE from 91 to 92. Words are transmitted order.
constexpr std::array<std::array<uint32_t,10>,7> frames{{
 {0x22c00012,0x215c0534,0x1cd9007b,0x3fffffff,0x3fffffff,0x3fffffff,0x3fffffff,0x3fffffff,0x3fffffff,0x3fffffac},
 {0x22c00012,0x215c21c8,0x00100023,0x3fffffff,0x3fffffff,0x3fffffff,0x3fffffff,0x2926fb19,0x3fc0247a,0x247c1484},
 {0x22c00012,0x215c4258,0x16c2eb4d,0x3cd3fe6e,0x26abc7c9,0x3d767f05,0x32a133f8,0x036b6842,0x034f4da3,0x26fb3f18},
 {0x22c00012,0x215c63d0,0x3ffd036b,0x257bbe0e,0x3ff80b5d,0x237214db,0x35cb2af3,0x2b5afec7,0x00117383,0x293ca340},
 {0x22c00012,0x215cc1ac,0x00100023,0x3fffffff,0x3fffffff,0x3fffffff,0x3fffffff,0x28e6fb39,0x3fc0247a,0x247c1484},
 {0x22c00012,0x215ce2c4,0x1702eb6d,0x3cd3fe6e,0x26abc7c9,0x3d767f05,0x32a133f8,0x036b6842,0x034f4da3,0x26fb3f18},
 {0x22c00012,0x215d0330,0x3ffd036b,0x257bbe0e,0x3ff80b5d,0x237214db,0x35cb2af3,0x2b5afec7,0x00117383,0x28fca360}
}};
Ephemeris sample_ephemeris() {
    Ephemeris e;
    e.week_mod1024=1; e.iodc=91; e.health=e.ura=0; e.sf1_tow=409920;
    e.toe=e.toc=409904; e.sqrt_a=5153.65489006; e.ecc=.00626740418375;
    e.crs=93.40625; e.delta_n=.116040547840e-8; e.m0=.162092304801;
    e.cuc=.484101474285e-5; e.cus=.652112066746e-5; e.cic=-.242143869400e-7; e.cis=-.596046447754e-7;
    e.omega0=.329237003460; e.i0=1.11541663136; e.crc=326.59375; e.omega=2.06958726335;
    e.omega_dot=-.638312302555e-8; e.idot=.307155651409e-9;
    e.af0=-.839701388031e-3; e.af1=-.165982783074e-10;
    return e;
}
void orbit_reference() {
    // Independent RTKLIB 71db0ff eph2pos. Velocities and clock drifts from
    // symmetric +/-25 ms difference. No code from either oracle is linked.
    constexpr double truth[3][8] = {
      {-5767334.870229526,-13447878.938652905,21967239.807803251,1083.5662489012,-2612.1176509932,-1317.8489457816,-.000839668567645293,-1.8691090342e-11},
      {-4237540.536484380,-18156232.315474905,18685002.295117114,548.1217299961,-2348.0459197611,-2151.5633717179,-.000839703718341767,-1.8676431929e-11},
      {-3656789.312784925,-22105020.713016920,13980594.583998583,91.2648175377,-1811.7884015292,-2820.9933074191,-.000839738691726452,-1.8503731533e-11}
    };
    auto e=sample_ephemeris();
    Satellite sat;
    for (int i=0;i<3;++i) {
        assert(propagate(e,e.toe+(i-1)*1880,sat));
        for (int j=0;j<3;++j) {
            assert(std::abs(sat.position_m[j]-truth[i][j])<.0001);
            assert(std::abs(sat.velocity_mps[j]-truth[i][j+3])<.0001);
        }
        assert(std::abs(sat.clock_s-truth[i][6])<1e-14);
        assert(std::abs(sat.clock_drift-truth[i][7])<1e-15);
    }
    assert(!propagate(e,e.toe+7201,sat)); e.health=1;
    assert(!propagate(e,e.toe,sat));
}
void message_test(bool invert) {
    LnavDecoder decoder;
    double now=0, tow=0;
    // An imposed code rate offset makes nominal-frequency TOW extrapolation
    // visibly wrong. Compare the fractional epoch within the final LNAV bit.
    const double period=sample_rate/1000 * (1+35e-6);
    for(int f=0;f<4;++f) {
        for (uint32_t word : frames[f]) for (int b=29;b>=0;--b) {
            bool sign = ((word>>b)&1u) ^ invert;
            for(int ms=0;ms<20;++ms) {
                decoder.push_prompt(sign ? -73. : 73.,now);
                now+=period;
            }
        }
    }
    assert(decoder.good_subframes()==3);
    const Ephemeris* e=decoder.ephemeris();
    assert(e && e->iodc==91 && e->week_mod1024==1 && e->toe==409904);
    assert(std::abs(e->sqrt_a - sample_ephemeris().sqrt_a)<.000002);
    assert(decoder.transmit_time(now-period*.3,tow));
    assert(std::abs(tow-(409938.-.0003))<1e-9);
    // Missing epoch invalidates bit sync and all associated time.
    decoder.push_prompt(1.,now+2*period);
    assert(!decoder.transmit_time(now,tow));
}
void parity_and_issue() {
    LnavDecoder decoder;
    double sample=0;
    auto feed=[&](unsigned frame, bool damage=false) {
        for (unsigned w=0;w<10;w++) for(int b=29;b>=0;--b) {
            bool bit=(frames[frame][w]>>b)&1u;
            decoder.push_bit(bit^(damage && w==3 && b==8),sample);
            sample+=.02*sample_rate;
        }
    };
    feed(1); feed(2,true); feed(3);
    assert(decoder.good_subframes()==2 && !decoder.ephemeris());
    decoder.reset(); sample=0;
    feed(1);feed(2);feed(3);
    assert(decoder.ephemeris() && decoder.ephemeris()->iodc==91);
    const auto saved = *decoder.ephemeris();
    decoder.reset();
    sample=0;
    assert(!decoder.can_use_cached(saved));
    feed(1);
    assert(decoder.can_use_cached(saved));
    auto old = saved;
    old.week_mod1024 = (saved.week_mod1024+1023)%1024;
    assert(!decoder.can_use_cached(old)); // repeated IODC in a previous week
    old = saved; old.toc -= 16;
    assert(!decoder.can_use_cached(old));
    feed(2);
    old = saved; old.toe -= 16;
    assert(!decoder.can_use_cached(old)); // orbit issue also repeats eventually
    feed(3);
    // Artificial data transition: provide intervening bits to maintain
    // continuous bit timing, then switch to a fresh, correctly parity-coded issue.
    for(int bit=0;bit<600;++bit) { decoder.push_bit(false,sample); sample+=.02*sample_rate; }
    feed(4); assert(!decoder.ephemeris() && !decoder.can_use_cached(saved));
    feed(5); assert(!decoder.ephemeris());
    feed(6); assert(decoder.ephemeris() && decoder.ephemeris()->iodc==92);
}
void independent_fix_at_rollover() {
    std::array<Ephemeris,6> eph{};
    std::array<Observation,6> obs{};
    eph::fixture::rollover_observations(eph,obs);
    const auto& at_zero=eph::fixture::at_zero;
    const auto& velocity=eph::fixture::velocity;
    Fix f;
    assert(solve_fix(obs.data(),obs.size(),f,.0021) && f.valid);
    for(int i=0;i<3;i++) {
        assert(std::abs(f.position_m[i] - (at_zero[i]+.002*velocity[i]))<.12);
        assert(std::abs(f.velocity_mps[i]-velocity[i])<.002);
    }
    assert(std::abs(f.clock_drift_mps-12)<.002);
    assert(f.week_mod1024==303 && std::abs(f.tow_s-.002)<1e-9);
    assert(std::abs(f.clock_bias_m - speed_of_light*.0001)<.12);
    // Data from two different broadcast weeks cannot be combined.
    --eph[0].week_mod1024;
    assert(!solve_fix(obs.data(),obs.size(),f,.0021) && !f.valid);
}
void bad_fix_clears_state() {
    Fix fix; fix.valid=true;
    assert(!solve_fix(nullptr,0,fix) && !fix.valid);
    auto eph=sample_ephemeris();
    std::array<Observation,4> obs{};
    for (unsigned i=0;i<obs.size();++i) obs[i]={&eph,eph.toe,0,i+1};
    assert(!solve_fix(obs.data(),obs.size(),fix)); // singular LOS
    obs[1].prn=obs[0].prn;
    assert(!solve_fix(obs.data(),obs.size(),fix));
}
void doppler_position() {
    PositionSmoother smoother;
    constexpr auto step = PositionSmoother::interval_samples;
    Fix fix;
    fix.valid = true;
    fix.velocity_mps = {10, -4, 0};
    fix.position_m = {100, 200, 300};
    smoother.update(fix, 0);
    assert((fix.position_m == Vec3{100, 200, 300}));

    // Constant acceleration in x, constant velocity in y, 40 m of code noise in z.
    fix.velocity_mps[0] = 12;
    fix.position_m = {102.2, 199.2, 340};
    smoother.update(fix, step);
    assert(std::abs(fix.position_m[0] - 102.2) < 1e-12);
    assert(std::abs(fix.position_m[1] - 199.2) < 1e-12);
    assert(fix.position_m[2] == 301 && fix.velocity_mps[0] == 12);

    // A dropped report, repeated or restarted clock, or explicit loss of fix
    // must seed directly from the next valid position.
    for (const auto sample : {3*step, 3*step, 0*step}) {
        fix.position_m = {50, 100, 150};
        smoother.update(fix, sample);
        assert((fix.position_m == Vec3{50, 100, 150}));
    }
    Fix invalid;
    smoother.update(invalid, step);
    assert(!invalid.valid);
    fix.position_m = {20, 40, 60};
    smoother.update(fix, step);
    assert((fix.position_m == Vec3{20, 40, 60}));
}

void velocity_fit() {
    constexpr auto step = PositionSmoother::interval_samples;
    VelocityFit filter;
    Fix f;
    f.valid = true;
    f.clock_drift_mps = 7;
    f.rate_rms_mps = 0.13;
    // A single impulse in x, constant acceleration in y, constant jerk in z.
    constexpr std::array<double, 5> impulse{{0.6, 0.4, 0.2, 0., -0.2}};
    for (unsigned i = 0; i < 11; ++i) {
        double t = i * .2;
        f.velocity_mps = {i == 5 ? 1. : 0., 7800. - 8. * t, 3. * t * t};
        filter.update(f, i * step);
        const double expected = i >= 5 && i <= 9 ? impulse[i - 5] : 0.;
        assert(std::abs(f.velocity_mps[0] - expected) < 1e-12);
        assert(std::abs(f.velocity_mps[1] - (7800. - 8. * t)) < 1e-10);
        assert(std::abs(f.velocity_mps[2] - (3. * t * t - (i >= 4 ? .24 : 0.))) < 1e-12);
        assert(f.clock_drift_mps == 7 && f.rate_rms_mps == .13);
    }
    // Each break discards the old window. Its first four new Doppler epochs pass through.
    for (auto start : {12 * step, 15 * step, 0 * step}) {
        for (unsigned i = 0; i < 4; ++i) {
            const Vec3 raw{double(i*i), 0, 0};
            f.velocity_mps = raw;
            filter.update(f, start + i * step);
            assert(f.velocity_mps == raw);
        }
    }
    f.valid = false;
    filter.update(f, 4 * step);
    f.valid = true;
    f.velocity_mps = {99, 98, 97};
    filter.update(f, 4 * step);
    assert((f.velocity_mps == Vec3{99, 98, 97}));
}
}
int main() {
    orbit_reference(); message_test(false); message_test(true);
    parity_and_issue(); independent_fix_at_rollover(); bad_fix_clears_state();
    doppler_position(); velocity_fit();
    std::puts("LNAV, RTKLIB broadcast state and navigation rejection tests passed");
}
