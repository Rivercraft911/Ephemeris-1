"""Export the 550 km ECEF trajectory or score the independent January 2022 orbital recording."""

import argparse
import json
from pathlib import Path

import numpy as np

from independent_static import check
from campaign import EARTH_RADIUS, circular


def trajectory(t):
    return circular(
        EARTH_RADIUS + 550000, np.deg2rad(97.6), np.deg2rad(35), np.deg2rad(12), t
    )


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    opts = cli.add_mutually_exclusive_group(required=True)
    opts.add_argument(
        "--motion", type=Path, help="write upstream ECEF motion CSV (10 Hz)"
    )
    opts.add_argument("--score", type=Path, help="score the 90 s receiver status CSV")
    args = cli.parse_args()
    if args.motion:
        t = np.arange(901) / 10
        with args.motion.open("x") as f:
            np.savetxt(
                f, np.column_stack((t, trajectory(t)[:, :3])), fmt="%.9f", delimiter=","
            )
    else:
        result = check(args.score, trajectory, 90)
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result["passed"] else 1)
