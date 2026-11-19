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

#ifdef EPHEMERIS_GROUND_IONOSPHERE
double GroundAtmosphere::ionosphere_slant_m(const Vec3& line, double tow_s,
                                           const Ionosphere& model) const {
    // IS-GPS-200N 20.3.3.5.2.5 and Table 20-X. Angles in the model are
    // semicircles. The cold model uses only the 5 ns vertical baseline.
    constexpr double pi = 3.14159265358979323846;
    const double sin_el = -(line[0]*up[0] + line[1]*up[1] + line[2]*up[2]);
    if (!(sin_el > 0)) return 0;
    const double elevation = std::asin(std::min(1., sin_el))/pi;
    const double q = .53 - elevation;
    const double slant = 1 + 16*q*q*q;
    constexpr double baseline_s = 5e-9;
    if (model.received_sample < 0) return speed_of_light*slant*baseline_s;

    const double longitude = std::atan2(up[1], up[0]);
    const double cos_latitude = std::hypot(up[0], up[1]);
    const double latitude = std::atan2(up[2], cos_latitude);
    const double cos_lon = std::cos(longitude), sin_lon = std::sin(longitude);
    const double east = line[0]*sin_lon - line[1]*cos_lon;
    const double north = (line[0]*cos_lon + line[1]*sin_lon)*up[2] - line[2]*cos_latitude;
    const double azimuth = std::atan2(east, north);
    const double psi = .0137/(elevation + .11) - .022;
    const double phi = std::clamp(latitude/pi + psi*std::cos(azimuth), -.416, .416);
    const double lambda = longitude/pi + psi*std::sin(azimuth)/std::cos(pi*phi);
    const double magnetic_latitude = phi + .064*std::cos(pi*(lambda - 1.617));
    double local_s = tow_s + 43200*lambda;
    local_s -= 86400*std::floor(local_s/86400);

    const auto& c = model.coefficients;
    const double m = magnetic_latitude;
    const double amplitude = c[0]*0x1p-30 + m*(c[1]*0x1p-27 + m*(c[2]*0x1p-24 + m*c[3]*0x1p-24));
    const double period = c[4]*0x1p11 + m*(c[5]*0x1p14 + m*(c[6]*0x1p16 + m*c[7]*0x1p16));
    const double phase = 2*pi*(local_s - 50400)/std::max(72000., period);
    double vertical_s = baseline_s;
    if (std::abs(phase) < 1.57) {
        const double phase_sq = phase*phase;
        vertical_s += std::max(0., amplitude)*(1 + phase_sq*(-.5 + phase_sq/24));
    }
    return speed_of_light*slant*vertical_s;
}
#endif
} // namespace eph::nav
