"""Score the known static January 2022 GPS-SDR-SIM recording. SPDX-License-Identifier: Apache-2.0"""

import argparse
import csv
import gzip
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from iqio import RATE
from campaign import Q


def check(path, trajectory=None, seconds=65, start_tow_s=525609):
    if seconds <= 0:
        raise ValueError("recording duration must be positive")
    open_csv = gzip.open if path.suffix == ".gz" else open
    with open_csv(path, "rt", newline="") as f:
        rows = list(csv.DictReader(f))
    t = np.array([int(r["sample"]) / RATE for r in rows])
    hz = Q["hz"]
    count = seconds * hz
    if len(t) != count or not np.allclose(
        t, (np.arange(count) + 1) / hz, rtol=0, atol=1e-8
    ):
        raise ValueError(
            f"expected all {count} status epochs from the {seconds}-second recording"
        )
    flags = np.array([int(r["valid"]) for r in rows])
    if np.any((flags != 0) & (flags != 1)):
        raise ValueError("unknown solution validity")
    fixes = [r for r in rows if int(r["valid"])]
    if not fixes:
        return {"passed": False, "fixes": 0}
    positions = np.array([[float(r[k]) for k in ("x_m", "y_m", "z_m")] for r in fixes])
    velocity = np.array(
        [[float(r[k]) for k in ("vx_mps", "vy_mps", "vz_mps")] for r in fixes]
    )
    if trajectory is None:
        ref_position, ref_velocity = [6378137, 0, 0], [0, 0, 0]
    else:
        truth = trajectory(t[flags == 1])
        ref_position, ref_velocity = truth[:, :3], truth[:, 3:]
    dp = np.linalg.norm(positions - ref_position, axis=1)
    dv = np.linalg.norm(velocity - ref_velocity, axis=1)
    time_error = max(
        abs(float(r["tow_s"]) - start_tow_s - int(r["sample"]) / RATE) for r in fixes
    )
    accurate = np.zeros(len(t), bool)
    accurate[flags == 1] = (dp <= Q["max_p95_position_m"]) & (dv <= Q["max_p95_velocity_mps"])
    required = Q["valid_streak"]
    streak = next((i for i in range(len(t) - required + 1) if np.all(accurate[i : i + required])), None)
    first = float(t[streak]) if streak is not None else None
    availability = float(np.mean(flags[streak:])) if streak is not None else 0
    p, v = float(np.percentile(dp, 95)), float(np.percentile(dv, 95))
    gaps = max(int(r["gaps"]) for r in rows)
    passed = bool(
        first is not None
        and first <= Q["max_ttff_s"]
        and p <= Q["max_p95_position_m"]
        and v <= Q["max_p95_velocity_mps"]
        and availability >= Q["min_availability"]
        and not gaps
        and time_error < 0.001
        and all(int(r["week_mod1024"]) == 142 for r in fixes)
    )
    return dict(
        passed=passed,
        status_epochs=len(t),
        valid_epochs=len(fixes),
        first_valid_s=float(t[flags == 1][0]),
        satellites_at_first_fix=int(fixes[0]["sv"]),
        first_fix_position_error_m=float(dp[0]),
        first_used_s={
            str(n): next((int(r["sample"]) / RATE for r in fixes if int(r["sv"]) >= n), None)
            for n in (6, 7, 8)
        },
        first_five_truth_checked_s=first,
        availability_after_streak=availability,
        p95_position_m=p,
        p95_velocity_mps=v,
        max_time_error_s=time_error,
        gaps=gaps,
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("receiver_csv", type=Path)
    p.add_argument("--seconds", type=int, default=65, help="expected recording duration")
    args = p.parse_args()
    result = check(args.receiver_csv, seconds=args.seconds)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
