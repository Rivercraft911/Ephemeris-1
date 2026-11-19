"""Compare production PVT to circular-orbit truth using independent light-time and range differences.
This test deliberately bypasses RF, acquisition, tracking and LNAV. It cannot score cold start.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import io
import json
import math
from pathlib import Path
import subprocess
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from campaign import EARTH_RADIUS, MU, OMEGA, Q, SPEC, circular, orbit_period

C = 299792458.0
L1 = 1575420000.0
GPS_RADIUS = 26_560_000.0


def measurements(rx, tow, node, phase):
    delay = np.full(len(tow), 0.075)
    for _ in range(4):
        sv = circular(GPS_RADIUS, math.radians(55), node, phase, tow - delay)[:, :3]
        rot = OMEGA * delay
        x = sv[:, 0] * np.cos(rot) + sv[:, 1] * np.sin(rot)
        y = -sv[:, 0] * np.sin(rot) + sv[:, 1] * np.cos(rot)
        delta = np.column_stack((x, y, sv[:, 2])) - rx[:, :3]
        distance = np.linalg.norm(delta, axis=1)
        delay = distance / C
    return delay, delta / distance[:, None]


def evaluate(probe, km, start, epochs, stress=False):
    tow0 = start["tow_s"]
    toe = float(16 * math.floor(tow0 / 16))
    incl = math.radians(SPEC["inclination_deg"])
    raan = math.radians(SPEC["raan_deg"])
    u = math.radians(start["phase_deg"])
    times = epochs / Q["hz"]
    tow = tow0 + times

    def rx(dt):
        return circular(EARTH_RADIUS + km * 1000, incl, raan, u, times + dt)

    truth = rx(0)
    before, after = rx(-0.05), rx(0.05)
    tx = np.empty((len(times), 24))
    fd = np.empty_like(tx)
    strength = np.empty_like(tx)
    header = ["24"]
    zenith = truth[:, :3] / np.linalg.norm(truth[:, :3], axis=1, keepdims=True)
    for i in range(24):
        plane, slot = divmod(i, 4)
        node = plane * math.pi / 3
        phase = slot * math.pi / 2 + plane * math.pi / 12
        af0 = (i - 12) * 1e-6
        af1 = (i - 10) * 1e-11
        M = math.remainder(phase + math.sqrt(MU / GPS_RADIUS**3) * toe, 2 * math.pi)
        header.append(
            " ".join(
                map(
                    str,
                    (
                        start["gps_week"] % 1024,
                        toe,
                        math.sqrt(GPS_RADIUS),
                        math.radians(55),
                        node,
                        M,
                        af0,
                        af1,
                    ),
                )
            )
        )
        tau, los = measurements(truth, tow, node, phase)
        first, _ = measurements(before, tow - 0.05, node, phase)
        last, _ = measurements(after, tow + 0.05, node, phase)
        tx[:, i] = tow - tau + af0 + af1 * (tow - tau - toe)
        # A seeded finite-difference observable independent of the C++ analytic derivative.
        fd[:, i] = (-(last - first) / 0.1 + af1 - 3e-9) * L1
        strength[:, i] = np.sum(los * zenith, axis=1)
    rows = []
    eligible = []
    clock = tow + 0.0004 + times * 3e-9
    for j, t in enumerate(times):
        visible = np.flatnonzero(strength[j] > math.sin(math.radians(5)))
        if stress and 10 <= t < 12:
            visible = visible[:3]
        vals = [f"{clock[j]:.11f} {len(visible)}"]
        for i in visible:
            vals.append(f"{i+1} {tx[j,i]:.11f} {fd[j,i]:.8f}")
        rows.append(" ".join(vals))
        eligible.append(len(visible) >= 6)
    proc = subprocess.run(
        [probe],
        input="\n".join(header + rows) + "\n",
        text=True,
        capture_output=True,
        check=True,
    )
    out = np.loadtxt(io.StringIO(proc.stdout), ndmin=2)
    assert len(out) == len(epochs)
    valid = out[:, 0] == 1
    if stress:
        assert not valid[(times >= 10) & (times < 12)].any()
    eligible = np.array(eligible)
    assert np.any(eligible), "fixture needs eligible geometry"
    assert np.all(valid[eligible]), "lost solution in eligible geometry"
    dp = np.linalg.norm(out[eligible, 1:4] - truth[eligible, :3], axis=1)
    dv = np.linalg.norm(out[eligible, 4:7] - truth[eligible, 3:], axis=1)
    # Deliberately tighter than RF limits: noiseless geometrical inputs.
    assert np.max(dp) < 0.15 and np.max(dv) < 0.02, (km, np.max(dp), np.max(dv))
    assert np.max(np.abs(out[eligible, 7] - tow[eligible])) < 1e-6
    return {
        "altitude_km": km,
        "phase_deg": start["phase_deg"],
        "epochs": len(epochs),
        "six_visible_epochs": int(eligible.sum()),
        "p95_m": float(np.percentile(dp, 95)),
        "p95_mps": float(np.percentile(dv, 95)),
        "max_abs_doppler_hz": float(np.max(np.abs(fd))),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe")
    parser.add_argument("--full", action="store_true")
    a = parser.parse_args()
    result = []
    for km in SPEC["altitudes_km"]:
        for ix, start in enumerate(SPEC["starts"]):
            duration = math.ceil(orbit_period(km)) if a.full and ix == 0 else 150
            epochs = (
                np.arange(duration * 5)
                if a.full
                else np.unique(np.r_[np.arange(0, 80), np.arange(100, 750, 35)])
            )
            result.append(evaluate(a.probe, km, start, epochs, stress=not a.full))
    print(
        json.dumps(
            {"level": "synthetic decoded observables", "scenarios": result}, indent=2
        )
    )
