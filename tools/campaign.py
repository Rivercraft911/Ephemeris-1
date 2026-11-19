"""Freeze LEO scenarios, generate host-only geometry, and score receiver CSV against truth.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import csv
import json
import math
from pathlib import Path

import numpy as np

SPEC_PATH = Path(__file__).resolve().parents[1] / "tests" / "scenarios.json"
SPEC = json.loads(SPEC_PATH.read_text())
Q = SPEC["qualification"]
MU = 3.986005e14
OMEGA = 7.2921151467e-5
EARTH_RADIUS = 6378137.0
FS = 16_368_000
TRUTH_HEADER = (
    "t_s",
    "x_m",
    "y_m",
    "z_m",
    "vx_mps",
    "vy_mps",
    "vz_mps",
    "healthy_sv",
    "pdop",
)
XYZ = TRUTH_HEADER[1:7]


def circular(radius, incl, node, phase, t):
    """Two-body position and analytic velocity in ECEF, meters and m/s."""
    n = math.sqrt(MU / radius**3)
    u, lon = phase + n * t, node - OMEGA * t
    c, s = np.cos(lon), np.sin(lon)
    cu, su = np.cos(u), np.sin(u)
    ci, si = np.cos(incl), np.sin(incl)
    x, y, z = (
        radius * (c * cu - s * su * ci),
        radius * (s * cu + c * su * ci),
        radius * su * si,
    )
    dx = radius * n * (-c * su - s * cu * ci) + OMEGA * y
    dy = radius * n * (-s * su + c * cu * ci) - OMEGA * x
    dz = radius * n * cu * si
    return np.stack((x, y, z, dx, dy, dz), axis=-1)


def orbit_period(km):
    return 2 * math.pi * math.sqrt((EARTH_RADIUS + 1000 * km) ** 3 / MU)


def geometry(km, start, t):
    """Synthetic visibility and DOP; RF acceptance must use the simulator's own truth."""
    r = circular(
        EARTH_RADIUS + 1000 * km,
        math.radians(SPEC["inclination_deg"]),
        math.radians(SPEC["raan_deg"]),
        math.radians(start["phase_deg"]),
        t,
    )
    sats = []
    for plane in range(6):
        for slot in range(4):
            # Representative GPS constellation for repeatable software geometry fixtures.
            s = circular(
                26_560_000,
                math.radians(55),
                plane * math.pi / 3,
                slot * math.pi / 2 + plane * math.pi / 12,
                t + start["tow_s"],
            )
            sats.append(s[:, :3])
    los = np.stack(sats, axis=1) - r[:, None, :3]
    los /= np.linalg.norm(los, axis=2, keepdims=True)
    zenith = r[:, :3] / np.linalg.norm(r[:, :3], axis=1, keepdims=True)
    visible = np.einsum("ijk,ik->ij", los, zenith) >= math.sin(math.radians(5))
    design = (
        np.concatenate((los, np.ones((*los.shape[:2], 1))), axis=2)
        * visible[:, :, None]
    )
    normal = design.transpose(0, 2, 1) @ design
    sv = visible.sum(axis=1)
    pdop = np.full(sv.shape, 999.0, dtype=float)
    ok = (sv >= 4) & (np.abs(np.linalg.det(normal)) > 1e-10)
    pdop[ok] = np.sqrt(np.trace(np.linalg.inv(normal[ok])[:, :3, :3], axis1=1, axis2=2))
    return r, sv, pdop


def prepare(out, full):
    out.mkdir(parents=True, exist_ok=True)
    manifest = []
    for altitude in SPEC["altitudes_km"]:
        for num, start in enumerate(SPEC["starts"]):
            name = f"leo-{altitude}-{num+1}"
            # First start at each altitude is also its full-orbit endurance run.
            duration = math.ceil(orbit_period(altitude)) if full and num == 0 else 150
            manifest.append(
                dict(id=name, altitude_km=altitude, duration_s=duration, **start)
            )
            with (out / (name + ".truth.csv")).open("w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(TRUTH_HEADER)
                for begin in range(0, duration * Q["hz"] + 1, 1000):
                    t = (
                        np.arange(begin, min(begin + 1000, duration * Q["hz"] + 1))
                        / Q["hz"]
                    )
                    states, num_sv, dop = geometry(altitude, start, t)
                    writer.writerows(np.column_stack((t, states, num_sv, dop)))
    (out / "campaign.json").write_text(
        json.dumps({"suite": SPEC, "runs": manifest}, indent=2) + "\n"
    )


def table(path):
    with Path(path).open(newline="") as f:
        return list(
            csv.DictReader(
                line for line in f if line.strip() and not line.startswith("#")
            )
        )


def score(
    truth, receiver, sample_zero_s, endurance=True, sample_rate_hz=FS, gps_start=None
):
    """Score the complete frozen 5 Hz grid, aligned by an external sample-zero marker."""
    if not math.isfinite(sample_zero_s):
        raise ValueError("sample-zero calibration must be finite")
    if (
        not math.isfinite(sample_rate_hz)
        or not 0.99 * FS <= sample_rate_hz <= 1.01 * FS
    ):
        raise ValueError("invalid calibrated ADC sample rate")
    stamp = np.array([float(r["t_s"]) for r in truth])
    cadence = 1 / Q["hz"]
    if (
        len(stamp) < 2
        or abs(stamp[0]) > 1e-7
        or not np.allclose(np.diff(stamp), cadence, rtol=0, atol=1e-7)
    ):
        raise ValueError("truth must be a complete 5 Hz grid from power-on")
    state = np.array([[float(r[k]) for k in XYZ] for r in truth])
    sv = np.array([float(r["healthy_sv"]) for r in truth])
    pdop = np.array([float(r["pdop"]) for r in truth])
    if not all(np.isfinite(a).all() for a in (state, sv, pdop)):
        raise ValueError("non-finite truth")
    eligible = (sv >= Q["min_healthy_sv"]) & (pdop <= Q["max_pdop"])
    fixed = np.zeros(len(stamp), dtype=bool)
    good = np.zeros_like(fixed)
    event_time = np.full(len(stamp), np.nan)
    seen = np.zeros_like(fixed)
    errors = np.full((len(stamp), 2), np.nan)
    rtn_errors = []
    faults = []
    previous = -1
    for row in receiver:
        sample = int(row["sample"])
        t = sample_zero_s + sample / sample_rate_hz
        if sample <= previous or sample < 0:
            faults.append("duplicate or out-of-order sample timestamp")
        previous = sample
        i = int(np.rint(t / cadence))
        if not 0 <= i < len(stamp):
            continue
        if seen[i]:
            faults.append("duplicate navigation epoch")
        seen[i] = True
        if int(row["gaps"]):
            faults.append("reported sample discontinuity")
        if row["valid"] not in ("0", "1"):
            raise ValueError("invalid solution flag")
        if row["valid"] == "0":
            continue
        if t < stamp[0] or t > stamp[-1]:
            continue
        got = np.array([float(row[k]) for k in XYZ])
        if not np.isfinite(got).all():
            faults.append("non-finite valid solution")
            continue
        expected = np.array([np.interp(t, stamp, state[:, k]) for k in range(6)])
        dp, dv = got[:3] - expected[:3], got[3:] - expected[3:]
        errors[i] = np.linalg.norm(dp), np.linalg.norm(dv)
        if gps_start is not None:
            week, tow = gps_start
            expected_tow = tow + t
            if (
                int(row["week_mod1024"]) != (week + int(expected_tow // 604800)) % 1024
                or abs(math.remainder(float(row["tow_s"]) - expected_tow, 604800))
                > 0.001
                or not math.isfinite(float(row["tow_s"]))
            ):
                faults.append("GPS epoch differs from independent truth")
        event_time[i] = t
        fixed[i] = True
        good[i] = (
            errors[i, 0] <= Q["max_p95_position_m"]
            and errors[i, 1] <= Q["max_p95_velocity_mps"]
        )
        if sv[i] < 4:
            faults.append("valid solution with fewer than four healthy SVs")
        if eligible[i]:
            radial = expected[:3] / np.linalg.norm(expected[:3])
            inertial_v = expected[3:] + np.cross([0.0, 0.0, OMEGA], expected[:3])
            normal = np.cross(radial, inertial_v)
            normal /= np.linalg.norm(normal)
            rtn_errors.append(
                [
                    float(dp @ axis)
                    for axis in (radial, np.cross(normal, radial), normal)
                ]
            )
    streak = Q["valid_streak"]
    ttff = None
    for i in range(len(stamp) - streak + 1):
        if np.all(good[i : i + streak] & eligible[i : i + streak]):
            ttff = float(event_time[i])
            break
    after = eligible & (np.arange(len(stamp)) >= (i if ttff is not None else 0))
    n = int(after.sum())
    used = after & fixed
    p95 = (
        np.percentile(errors[used], 95, axis=0).tolist() if used.any() else [None, None]
    )
    availability = float(used.sum() / n) if n else 0.0
    if ttff is None or ttff > Q["max_ttff_s"]:
        faults.append("unaided cold-start time")
    if endurance and (
        availability < Q["min_availability"]
        or not used.any()
        or p95[0] > Q["max_p95_position_m"]
        or p95[1] > Q["max_p95_velocity_mps"]
    ):
        faults.append("nominal orbit accuracy or availability")
    if np.any(after & ~seen):
        faults.append("missing 5 Hz status after first fix")
    sub = np.abs(rtn_errors)
    return dict(
        passed=not faults,
        ttff_s=ttff,
        eligible_epochs=n,
        valid_epochs=int(used.sum()),
        availability=availability,
        p95_position_m=p95[0],
        p95_velocity_mps=p95[1],
        p95_abs_rtn_m=np.percentile(sub, 95, axis=0).tolist() if len(sub) else None,
        faults=sorted(set(faults)),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    cmds = parser.add_subparsers(dest="command", required=True)
    prep = cmds.add_parser("prepare")
    prep.add_argument("out", type=Path)
    prep.add_argument("--full-orbits", action="store_true")
    run = cmds.add_parser("score")
    run.add_argument("truth", type=Path)
    run.add_argument("receiver", type=Path)
    run.add_argument(
        "--sample-zero-s",
        type=float,
        required=True,
        help="independently measured t at ADC sample 0",
    )
    run.add_argument(
        "--sample-rate-hz",
        type=float,
        default=FS,
        help="measured ADC sample rate from marker calibration",
    )
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.out, args.full_orbits)
        return
    result = score(
        table(args.truth),
        table(args.receiver),
        args.sample_zero_s,
        sample_rate_hz=args.sample_rate_hz,
    )
    print(json.dumps(result, indent=2, allow_nan=False))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
