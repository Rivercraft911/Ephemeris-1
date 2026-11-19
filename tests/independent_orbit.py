"""Export an ECEF trajectory or score an independent January 2022 orbital recording."""

import argparse
import json
from pathlib import Path

import numpy as np

from independent_static import check
from campaign import EARTH_RADIUS, circular


def trajectory(t, altitude_km=550, phase_deg=12):
    return circular(
        EARTH_RADIUS + 1000 * altitude_km, np.deg2rad(97.6), np.deg2rad(35), np.deg2rad(phase_deg), t
    )


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    opts = cli.add_mutually_exclusive_group(required=True)
    opts.add_argument(
        "--motion", type=Path, help="write upstream ECEF motion CSV (10 Hz)"
    )
    opts.add_argument("--score", type=Path, help="score the receiver status CSV")
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
    else:
        result = check(args.score, motion, args.seconds)
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result["passed"] else 1)
