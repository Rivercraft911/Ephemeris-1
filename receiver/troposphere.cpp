// SPDX-License-Identifier: Apache-2.0
#include "troposphere.hpp"
#include <algorithm>
#include <cmath>

namespace eph::nav {
bool GroundAtmosphere::set_from_ecef(const Vec3& r) {
    constexpr double a = 6378137.0, flattening = 1.0 / 298.257223563;
    constexpr double b = a * (1 - flattening), e2 = flattening * (2 - flattening);
    constexpr double low = -500, high = 9000;
    zenith_m = 0;
    const double xy2 = r[0]*r[0] + r[1]*r[1];
    const double r2 = xy2 + r[2]*r[2];
    if (!(r2 >= (b + low)*(b + low) && r2 <= (a + high)*(a + high))) return false;

    // Bowring's geodetic normal in the meridian plane, including both poles.
    const double xy = std::sqrt(xy2);
    const double z = r[2] * (a/b);
    const double beta_norm = std::sqrt(xy2 + z*z);
    const double sb = z/beta_norm, cb = xy/beta_norm;
    const double north = r[2] + b * e2/(1-e2) * sb*sb*sb;
    const double outward = xy - a * e2 * cb*cb*cb;
    const double norm = std::sqrt(north*north + outward*outward);
    const double sin_lat = north/norm, cos_lat = outward/norm;
    const double h = xy*cos_lat + r[2]*sin_lat - a*std::sqrt(1-e2*sin_lat*sin_lat);
    if (h < low || h > high) return false;
    const double scale = xy > 0 ? cos_lat/xy : 0;
    up = {r[0]*scale, r[1]*scale, sin_lat};

    // Saastamoinen standard atmosphere. Pressure and vapour pressure are hPa,
    // temperature is K and h is ellipsoidal height in m (MSL approximation).
    const double p = 1013.15 * std::pow(1 - 2.2557e-5*h, 5.2568);
    const double t = 288.15 - .0065*h;
    const double e = .70 * 6.108 * std::exp((17.15*t-4684)/(t-38.45));
    zenith_m = .002277 * (p + (1255/t + .05)*e);
    return true;
}

double GroundAtmosphere::slant_m(const Vec3& line) const {
    const double sin_el = -(line[0]*up[0] + line[1]*up[1] + line[2]*up[2]);
    if (!(sin_el > 0)) return 0;
    constexpr double sin_five_deg = .08715574274765817;
    const double sec_z = 1.0 / std::clamp(sin_el, sin_five_deg, 1.0);
    return (zenith_m - .002277*(sec_z*sec_z-1.0)) * sec_z;
}
} // namespace eph::nav
