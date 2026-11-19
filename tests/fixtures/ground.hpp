// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "receiver/navigation.hpp"
#include <cmath>

namespace eph::fixture {
inline constexpr nav::Vec3 ground_position{6378137, 0, 0};
inline constexpr double ground_tow = 345595;

inline void ground_observations(std::array<nav::Ephemeris,6>& eph,
                                std::array<nav::Observation,6>& obs) {
    // sky_signal.geometry(967,6,0). Transmit times include a 2.4 m zenith
    // delay with Black–Eisner mapping. Doppler differentiates total delay at ±50 ms.
    // Each row is broadcast node (rad), anomaly (rad), observed SV time (s), Doppler (Hz).
    constexpr double data[][4] = {
        {-1.98910964406935, 3.0521209876272, 345594.920165295654442, -188.548952288908},
        {-.589269616804813, 1.22025194930595, 345594.922320332843810, -2868.681681925144},
        {2.8630117896929, 2.6506502579027, 345594.925678011553828, 1973.970821206344},
        {-.172361803926692, -.756327385945737, 345594.922574597701896, 2732.514094317578},
        {1.46651945442175, -1.17839138233103, 345594.921991555951536, 475.315394449921},
        {.0860794807787465, .241372614869685, 345594.931810224486981, -1087.436165028263}
    };
    for (unsigned i=0; i<eph.size(); ++i) {
        auto& e = eph[i];
        e = {};
        e.week_mod1024 = 303;
        e.health = e.ura = 0;
        e.iodc = 1;
        e.sf1_tow = ground_tow - 30;
        e.toe = e.toc = 345600;
        e.sqrt_a = 5153.6394901275635;
        e.i0 = .9599310884343347;
        e.omega0 = data[i][0];
        e.m0 = data[i][1];
        e.af0 = std::ldexp(double(1800*(int(i)+1)-5500), -31);
        obs[i] = {&e, data[i][2], data[i][3], i+1};
    }
}
} // namespace eph::fixture
