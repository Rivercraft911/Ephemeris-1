"""Compare a causal 5 Hz position estimate with the same raw fixes and orbital truth.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from campaign import FS, Q, XYZ, table


def estimate(rows):
    """Independent reference for the firmware's Doppler-aided 8 s position estimate."""
    prior_sample, prior_gaps = None, None
    position = velocity = None
    for row in rows:
        sample, gaps = int(row["sample"]), int(row["gaps"])
        if row["valid"] == "0":
            prior_sample = None
            yield None
            continue
        if row["valid"] != "1":
            raise ValueError("invalid fix flag")
        raw = np.array([float(row[key]) for key in XYZ[:3]])
        current_velocity = np.array([float(row[key]) for key in XYZ[3:]])
        if prior_sample is None or sample - prior_sample != FS // 5 or gaps != prior_gaps:
            position = raw
        else:
            predicted = position + 0.1 * (velocity + current_velocity)
            position = predicted + (raw - predicted) / 40
        prior_sample, prior_gaps, velocity = sample, gaps, current_velocity
        yield position


def lock_times(rows):
    def first(field, n):
        return next((int(r["sample"]) / FS for r in rows if bin(int(r[field])).count("1") >= n), None)

    return {"one_lock_s": first("locked_mask", 1), "four_locks_s": first("locked_mask", 4),
            "four_navigation_s": first("nav_mask", 4), "first_valid_s": first("valid", 1)}


def compare(receiver, truth, start_s=0.0):
    rows = table(receiver)
    reference = {
        round(float(row["t_s"]) * FS): np.array([float(row[key]) for key in XYZ[:3]])
        for row in table(truth)
        if float(row["healthy_sv"]) >= Q["min_healthy_sv"] and float(row["pdop"]) <= Q["max_pdop"]
    }
    errors = []
    for row, position in zip(rows, estimate(rows)):
        sample = int(row["sample"])
        if position is None or sample < round(start_s * FS) or sample not in reference:
            continue
        raw = np.array([float(row[key]) for key in XYZ[:3]])
        errors.append([np.linalg.norm(raw - reference[sample]),
                       np.linalg.norm(position - reference[sample])])
    if not errors:
        raise ValueError("no matched valid, eligible fixes")
    return {
        **lock_times(rows), "scored_from_s": start_s, "paired_fixes": len(errors),
        "raw_p95_m": float(np.percentile(np.asarray(errors)[:, 0], 95)),
        "aided_p95_m": float(np.percentile(np.asarray(errors)[:, 1], 95)),
        "receiver_csv_sha256": hashlib.sha256(Path(receiver).read_bytes()).hexdigest(),
        "truth_csv_sha256": hashlib.sha256(Path(truth).read_bytes()).hexdigest(),
    }


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("receiver", type=Path)
    cli.add_argument("truth", type=Path)
    cli.add_argument("--start-s", type=float, default=0,
                     help="first scored truth time, e.g. the baseline qualification TTFF")
    args = cli.parse_args()
    print(json.dumps(compare(args.receiver, args.truth, args.start_s), indent=2))
