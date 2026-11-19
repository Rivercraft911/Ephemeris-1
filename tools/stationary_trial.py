"""Score a causal stationary-antenna mean on the same 5 Hz epochs as raw fixes.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path

import numpy as np

from iqio import RATE


def stationary_estimates(rows):
    """Yield valid raw and averaged positions with elapsed continuous lock time."""
    count = 0
    previous = None
    for row in rows:
        sample, gaps = int(row["sample"]), int(row["gaps"])
        if sample < 0 or gaps < 0 or row["valid"] not in ("0", "1"):
            raise ValueError("invalid receiver status")
        if previous != (sample - RATE // 5, gaps):
            count = 0
        previous = (sample, gaps)
        if row["valid"] == "0":
            count = 0
            continue
        raw = np.array([float(row[key]) for key in ("x_m", "y_m", "z_m")])
        if not np.isfinite(raw).all():
            raise ValueError("non-finite valid position")
        count += 1
        if count == 1:
            first, mean = sample, raw.copy()
        else:
            mean += (raw - mean) / count
        yield sample, (sample - first) / RATE, raw, mean.copy()


def error_stats(positions, reference):
    delta = np.asarray(positions) - reference
    error = np.linalg.norm(delta, axis=1)
    return {
        "rms_3d_m": float(np.sqrt(np.mean(error**2))),
        "p95_3d_m": float(np.percentile(error, 95)),
        "mean_ecef_error_m": delta.mean(axis=0).tolist(),
    }


def score(path, reference, warmup_s):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    fixes = list(stationary_estimates(rows))
    paired = [fix for fix in fixes if fix[1] >= warmup_s]
    result = {
        "receiver_csv_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "status_epochs": len(rows),
        "valid_epochs": len(fixes),
        "scored_epochs": len(paired),
        "first_fix_s": fixes[0][0] / RATE if fixes else None,
        "first_scored_s": paired[0][0] / RATE if paired else None,
    }
    if paired:
        result["raw"] = error_stats([fix[2] for fix in paired], reference)
        result["stationary_mean"] = error_stats([fix[3] for fix in paired], reference)
    return result


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("files", type=Path, nargs="+", help="native receiver CSV files")
    cli.add_argument("--truth-ecef-m", type=float, nargs=3, required=True)
    cli.add_argument("--warmup-s", type=float, default=10)
    args = cli.parse_args()
    if not math.isfinite(args.warmup_s) or args.warmup_s < 0:
        cli.error("warmup must be finite and nonnegative")
    if not np.isfinite(args.truth_ecef_m).all():
        cli.error("truth position must be finite")
    names = [path.stem for path in args.files]
    if len(names) != len(set(names)):
        cli.error("each input must have a distinct file stem")
    print(json.dumps({
        "warmup_s": args.warmup_s,
        "truth_ecef_m": args.truth_ecef_m,
        "runs": {
            path.stem: score(path, args.truth_ecef_m, args.warmup_s)
            for path in args.files
        },
    }, indent=2, allow_nan=False))
