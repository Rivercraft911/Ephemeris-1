// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "navigation.hpp"

namespace eph::nav {
struct GroundAtmosphere {
    Vec3 up{};
    double zenith_m{};
    bool set_from_ecef(const Vec3& position);
    double slant_m(const Vec3& satellite_to_receiver_unit) const;
};
} // namespace eph::nav
