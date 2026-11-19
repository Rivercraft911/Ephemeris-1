// SPDX-License-Identifier: Apache-2.0
#include "receiver/troposphere.hpp"
#include "tests/fixtures/ground.hpp"
#include "tests/fixtures/rollover.hpp"
#include <array>
#include <cassert>
#include <cmath>

using namespace eph::nav;
namespace {
using Frame = std::array<uint32_t,10>;
// Encoded with sky_signal.word's independent bit-position parity equations.
constexpr Frame page18{0x22c00012,0x1dffe47c,0x1e05c08e,0x3fbfcef8,0x00bf0091,
                       0x3fffffd6,0x00000029,0x3fffffd6,0x00000029,0x3fffff20};
constexpr std::array<int8_t,8> bytes{23,2,-2,-1,59,2,-4,2};
void feed(LnavDecoder& d, const Frame& frame, double start = 0) {
    for (const auto word : frame) for (int bit=29; bit>=0; --bit) {
        d.push_bit((word>>bit)&1u, start);
        start += sample_rate*.02;
    }
}
void decode() {
    LnavDecoder d;
    feed(d, page18);
    const auto& page = d.ionosphere();
    assert(d.good_subframes()==1 && page.coefficients==bytes);
    assert(page.fresh(page.received_sample) && !page.fresh(page.received_sample-1));
    assert(!page.fresh(page.received_sample + 86400*sample_rate));

    auto invalid = page18;
    invalid[2] ^= 1u;
    d.reset(); feed(d,invalid);
    assert(!d.good_subframes() && d.ionosphere().received_sample < 0);
    invalid = page18;
    invalid[1] = 0x1e0005c4; // parity-valid subframe 5
    d.reset(); feed(d,invalid);
    assert(d.good_subframes()==1 && d.ionosphere().received_sample < 0);
    constexpr Frame other_page{0x22c00012,0x1dffe47c,0x1dc5c081,0x00403107,0x3f40ff6e,
                               0x00000029,0x3fffffd6,0x00000029,0x3fffffd6,0x0000008c};
    constexpr Frame other_id{0x22c00012,0x1dffe47c,0x2e05c090,0x3fbfced1,0x3f40ff47,
                             0x3fffffff,0x3fffffff,0x3fffffff,0x3fffffff,0x3fffffac};
    for (const auto& frame : {other_page,other_id}) {
        d.reset(); feed(d,frame);
        assert(d.good_subframes()==1 && d.ionosphere().received_sample < 0);
    }
    d.reset(); feed(d,page18);
    invalid = page18;
    invalid[1] = 0x1dfff4b8; // valid HOW parity, alert flag set
    feed(d,invalid,6*sample_rate);
    assert(d.ionosphere().received_sample < 0);
}
void reference() {
    // RTKLIB 180043e, ionmodel(gpst2time(1734,TOW), coefficients, LLH, azel).
    // Input angles in degrees, reference slant group delay in meters.
    constexpr double cases[][6] = {
        {41.27,1.98,137,27,368658,4.783733445160},
        {41.27,1.98,137,27,396000,13.005409903858},
        {-58,-172,280,5,120000,4.537037115716},
        {85,156,15,30,360000,6.377939546573},
    };
    constexpr double rad = 3.14159265358979323846/180;
    GroundAtmosphere ground;
    const Ionosphere broadcast{bytes,0};
    for (const auto& r : cases) {
        const double lat = r[0]*rad, lon = r[1]*rad, az=r[2]*rad, el=r[3]*rad;
        ground.up = {std::cos(lat)*std::cos(lon),std::cos(lat)*std::sin(lon),std::sin(lat)};
        const Vec3 east{-std::sin(lon),std::cos(lon),0};
        const Vec3 north{-std::sin(lat)*std::cos(lon),-std::sin(lat)*std::sin(lon),std::cos(lat)};
        Vec3 line;
        for (unsigned i=0; i<3; ++i)
            line[i] = -(std::cos(el)*(std::sin(az)*east[i]+std::cos(az)*north[i]) + std::sin(el)*ground.up[i]);
        assert(std::abs(ground.ionosphere_slant_m(line,r[4],broadcast) - r[5]) < 1e-9);
    }
    assert(ground.set_from_ecef({6378137,0,0}));
    assert(std::abs(ground.ionosphere_slant_m({-1,0,0},50400,{}) - 1.49960984171) < 1e-8);
    assert(ground.ionosphere_slant_m({1,0,0},50400,broadcast)==0);
}
void corrected_code_observations() {
    std::array<Ephemeris,6> eph;
    std::array<Observation,6> obs;
    eph::fixture::ground_observations(eph,obs);
    Fix clean, corrected, uncorrected;
    constexpr double tow = eph::fixture::ground_tow;
    assert(solve_fix(obs.data(),obs.size(),clean,tow));
    // RTKLIB ionmodel at each fixture satellite's azimuth and elevation.
    constexpr double delays_m[] = {3.440886028,2.974420589,2.347445432,2.923004245,3.042896839,1.557514005};
    for (unsigned i=0; i<obs.size(); ++i) obs[i].sv_tow_s -= delays_m[i]/speed_of_light;
    const Ionosphere broadcast{bytes,0};
    assert(solve_fix(obs.data(),obs.size(),corrected,tow,&broadcast));
    assert(solve_fix(obs.data(),obs.size(),uncorrected,tow));
    // At this equatorial antenna ECEF X points up. Double-precision TOW at
    // this week epoch resolves 1.75 cm in code range before geometry dilution.
    assert(std::abs(uncorrected.position_m[0]-clean.position_m[0]) > 1);
    for (unsigned i=0; i<3; ++i)
        assert(std::abs(corrected.position_m[i]-clean.position_m[i]) < .10);
}
void orbital_observations() {
    std::array<Ephemeris,6> eph;
    std::array<Observation,6> obs;
    eph::fixture::rollover_observations(eph,obs);
    Fix off, on;
    const Ionosphere broadcast{bytes,0};
    assert(solve_fix(obs.data(),obs.size(),off));
    assert(solve_fix(obs.data(),obs.size(),on,std::numeric_limits<double>::quiet_NaN(),&broadcast));
    assert(off.position_m==on.position_m && off.velocity_mps==on.velocity_mps);
}
}
int main() {
    decode();
    reference();
    corrected_code_observations();
    orbital_observations();
}
