// SPDX-License-Identifier: Apache-2.0
#include "receiver/troposphere.hpp"
#include "tests/fixtures/ground.hpp"
#include <cassert>
#include <cmath>

int main() {
    namespace nav = eph::nav;
    nav::GroundAtmosphere air;
    assert(air.set_from_ecef({6378137,0,0}));
    assert(std::abs(air.slant_m({-1,0,0}) - 2.4273566195) < 1e-9);
    assert(std::abs(air.slant_m({-.5,std::sqrt(.75),0}) - 4.8410512390) < 1e-9);
    assert(air.slant_m({.5,std::sqrt(.75),0}) == 0);
    // WGS84 N 45°, E 30°, ellipsoidal height 1000 m; the elevation normal is geodetic.
    assert(air.set_from_ecef({3912960.837423739,2259148.992815059,4488055.515647106}));
    assert(std::abs(air.up[2] - std::sqrt(.5)) < 1e-10);
    assert(std::abs(air.zenith_m - 2.1262619938) < 1e-8);
    assert(air.set_from_ecef({0,0,6356752.314245}));
    assert(air.up[2] == 1 && std::abs(air.zenith_m - 2.42735662) < 1e-8);
    assert(!air.set_from_ecef({6778137,0,0}) && air.zenith_m == 0);
    assert(!air.set_from_ecef({0,0,0}));

    std::array<nav::Ephemeris,6> eph;
    std::array<nav::Observation,6> obs;
    eph::fixture::ground_observations(eph,obs);
    nav::Fix f;
    assert(nav::solve_fix(obs.data(),obs.size(),f,eph::fixture::ground_tow));
    double error_sq = 0, velocity_sq = 0;
    for (unsigned j=0; j<3; ++j) {
        const double d = f.position_m[j] - eph::fixture::ground_position[j];
        error_sq += d*d;
        velocity_sq += f.velocity_mps[j]*f.velocity_mps[j];
    }
    assert(f.week_mod1024 == 303 && f.satellites == 6 && error_sq < .25 && velocity_sq < .0001);
}
