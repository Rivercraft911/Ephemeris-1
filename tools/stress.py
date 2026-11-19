"""Deterministic conducted-signal stress cases from the frozen LEO suite.
SPDX-License-Identifier: Apache-2.0
"""

import math
import numpy as np
from campaign import SPEC


class Stress:
    def __init__(self, name=None):
        self.config = (
            {}
            if not name or name == "nominal"
            else next((c for c in SPEC["stress"] if c["id"] == name), None)
        )
        if self.config is None:
            raise ValueError(f"unknown stress case {name!r}")
        self.clock_scale = 1 + self.config.get("ppm", 0) * 1e-6
        self._faded_prn = None

    def amplitudes(self, t_s, prns, elevations_rad, amplitudes, azimuths_rad=None):
        a = np.array(amplitudes, copy=True)
        c = self.config
        if not c.get("start_s", math.inf) <= t_s < c.get("end_s", -math.inf):
            return a
        kind = c["type"]
        if kind == "disable_all_sv":
            a[:] = 0
        elif kind == "limit_healthy_sv":
            a[np.argsort(-a, kind="stable")[c["count"] :]] = 0
        elif kind == "attenuate_strongest_sv":
            if self._faded_prn is None and a.size and np.max(a) > 0:
                self._faded_prn = int(prns[np.argmax(a)])
            a[np.asarray(prns) == self._faded_prn] *= 10 ** (-c["loss_db"] / 20)
        elif kind == "rotate_antenna":
            if azimuths_rad is None:
                raise ValueError("tumble needs per-SV azimuth")
            theta = 2 * math.pi * (t_s - c["start_s"]) / c["period_s"]
            # Tilt a zenith boresight towards local east. Voltage gain is the
            # forward dot product, floored at -30 dB for the rear hemisphere.
            dot = np.cos(theta) * np.sin(elevations_rad) + np.sin(theta) * np.cos(
                elevations_rad
            ) * np.sin(azimuths_rad)
            a *= np.clip(dot, 10 ** (-30 / 20), 1)
        return a
