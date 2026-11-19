"""Export an ECEF trajectory or score an independent January 2022 orbital recording."""

import argparse
import json
from pathlib import Path

import numpy as np

from independent_static import check
from campaign import EARTH_RADIUS, FS, Q, XYZ, circular, table


def trajectory(t, altitude_km=550, phase_deg=12):
    return circular(
        EARTH_RADIUS + 1000 * altitude_km, np.deg2rad(97.6), np.deg2rad(35), np.deg2rad(phase_deg), t
    )


def compare_positions(reference, candidate, motion, seconds):
    """Paired 3D errors for identical epochs and satellite geometry."""
    first, second = table(reference), table(candidate)
    hz = Q["hz"]
    if len(first) != seconds * hz or len(second) != len(first):
        raise ValueError("both runs must contain the complete 5 Hz status stream")
    paired = []
    for number, (a, b) in enumerate(zip(first, second), start=1):
        sample = number * (FS // hz)
        if int(a["sample"]) != sample or int(b["sample"]) != sample:
            raise ValueError(f"sample mismatch at report {number}")
        if a["valid"] not in ("0", "1") or a["valid"] != b["valid"]:
            raise ValueError(f"fix availability differs at report {number}")
        if a["valid"] == "0":
            continue
        if int(a["nav_mask"]) != int(b["nav_mask"]) or a["sv"] != b["sv"]:
            raise ValueError(f"different satellites contributed at report {number}")
        paired.append((a, b))
    if not paired:
        raise ValueError("neither recording produced a fix")
    times = np.array([int(a["sample"]) / FS for a, _ in paired])
    truth = motion(times)[:, :3]
    result = {"paired_fixes": len(paired), "first_fix_s": float(times[0])}
    for name, rows in zip(("reference", "candidate"), zip(*paired)):
        estimates = np.array([[float(row[k]) for k in XYZ[:3]] for row in rows])
        if not np.isfinite(estimates).all():
            raise ValueError(f"nonfinite {name} position")
        errors = np.linalg.norm(estimates - truth, axis=1)
        result[name + "_p95_m"] = float(np.percentile(errors, 95))
    return result


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    opts = cli.add_mutually_exclusive_group(required=True)
    opts.add_argument(
        "--motion", type=Path, help="write upstream ECEF motion CSV (10 Hz)"
    )
    opts.add_argument("--score", type=Path, help="score the receiver status CSV")
    opts.add_argument("--compare-position", nargs=2, type=Path, metavar=("REFERENCE", "CANDIDATE"),
                      help="compare positions when every valid epoch uses the same satellites")
    cli.add_argument("--seconds", type=int, default=90, help="recording duration")
    cli.add_argument("--altitude-km", type=float, default=550, help="circular orbit altitude")
    cli.add_argument("--phase-deg", type=float, default=12, help="initial argument of latitude")
    args = cli.parse_args()
    if args.seconds <= 0:
        cli.error("recording duration must be positive")
    if not np.isfinite(args.altitude_km) or args.altitude_km <= 0 or not np.isfinite(args.phase_deg):
        cli.error("use a finite phase and a positive finite orbit altitude")

    def motion(t):
        return trajectory(t, args.altitude_km, args.phase_deg)

    if args.motion:
        t = np.arange(10 * args.seconds + 1) / 10
        with args.motion.open("x") as f:
            np.savetxt(
                f, np.column_stack((t, motion(t)[:, :3])), fmt="%.9f", delimiter=","
            )
    elif args.compare_position:
        result = compare_positions(*args.compare_position, motion, args.seconds)
        print(json.dumps(result, indent=2))
    else:
        result = check(args.score, motion, args.seconds)
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result["passed"] else 1)
