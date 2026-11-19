// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "receiver/navigation.hpp"
#include <cmath>

namespace eph::fixture {
inline constexpr nav::Vec3 velocity{-2610.1,452.8,6904.4};
inline constexpr nav::Vec3 at_zero{5987743.125,3012940.774,1876100.46};
// Observations borrow the ephemerides supplied by the caller.
inline void rollover_observations(std::array<nav::Ephemeris,6>& eph,
                                  std::array<nav::Observation,6>& obs) {
    // RTKLIB eph2pos generated the six satellite clocks and light-times at
    // GPS week 2351, TOW 0.002. Doppler is a centered 100 ms finite difference
    // of full light-time + clock. ECEF truth varies linearly over that span.
    // Transmit times are all in the previous week. Include receiver oscillator
    // bias and drift, and positive/negative SV group delays.
    constexpr double truth[6][2] = {
        {604799.935902604368, 11170.492686069}, {604799.894054020173, 18729.441670919},
        {604799.914297181880, -36153.693438338}, {604799.902982773026, 16139.752382503},
        {604799.920982608572, 22030.242406032}, {604799.916286747670, -26050.123752630}
    };
    for (unsigned j=0; j<eph.size(); ++j) {
        auto& e=eph[j];
        const int index=int(j);
        e.week_mod1024=302; e.sf1_tow=604770; e.iodc=17; e.health=e.ura=0;
        e.sqrt_a=std::sqrt(26560000.); e.ecc=.008+.0001*j;
        e.omega0=.8*j; e.i0=.97; e.omega=.21; e.m0=.47+1.92*j;
        e.omega_dot=-8e-9; e.idot=2e-10;
        e.af0=(index-2)*.00007; e.af1=(index-3)*1e-11; e.tgd=(index-4)*1e-9;
        obs[j]={&e,truth[j][0],truth[j][1]-12*nav::l1_frequency/nav::speed_of_light,j+1};
    }
}
}
