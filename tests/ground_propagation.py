"""Score a static equatorial raw-I/Q ground replay. SPDX-License-Identifier: Apache-2.0"""

import argparse
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from campaign import FS, XYZ, table


def summary(truth, rows):
    """Use every valid fix, including valid fixes outside the accuracy target."""
    grid = np.array([float(r["t_s"]) for r in truth])
    reference = np.array([[float(r[k]) for k in XYZ] for r in truth])
    if not np.all(reference == [6378137, 0, 0, 0, 0, 0]) or not np.allclose(
        grid, np.arange(len(grid)) / 5, rtol=0, atol=1e-9
    ):
        raise ValueError("expected the static equatorial fixture with a complete 5 Hz truth grid")
    sample = np.array([int(r["sample"]) for r in rows])
    if not np.array_equal(sample, np.rint(grid[1:] * FS).astype(np.int64)):
        raise ValueError("missing or misaligned 5 Hz receiver status")
    times = grid[1:]
    valid = np.array([int(r["valid"]) for r in rows])
    if not np.all(np.isin(valid, [0, 1])):
        raise ValueError("invalid fix flag")
    used = valid.astype(bool)
    locked = np.array([int(r["locked_mask"], 0).bit_count() for r in rows])
    decoded = np.array([int(r["nav_mask"], 0).bit_count() for r in rows])

    def first(flag):
        return float(times[flag][0]) if np.any(flag) else None

    result = {
        "status_epochs": len(rows),
        "first_reported_lock_s": first(locked > 0),
        "first_four_reported_locks_s": first(locked >= 4),
        "first_four_decoded_ephemerides_s": first(decoded >= 4),
        "first_valid_s": first(used),
        "valid_epochs": int(used.sum()),
        "max_cumulative_gaps": max(int(r["gaps"]) for r in rows),
    }
    if not np.any(used):
        return result
    if any(
        int(row["week_mod1024"]) != int(ref["gps_week"]) % 1024
        or abs(float(row["tow_s"]) - float(ref["tow_s"])) > 0.001
        for row, ref, valid_fix in zip(rows, truth[1:], used)
        if valid_fix
    ):
        raise ValueError("valid fix has incorrect GPS time")
    error = np.array([[float(r[k]) for k in XYZ] for r in rows])[used] - reference[1:][used]
    if not np.isfinite(error).all():
        raise ValueError("nonfinite valid fix")
    position = np.linalg.norm(error[:, :3], axis=1)
    velocity = np.linalg.norm(error[:, 3:], axis=1)
    near = used.copy()
    near[used] = (position <= 10) & (velocity <= 0.2)
    streak = np.convolve(near.astype(int), np.ones(5, dtype=int), "valid") == 5
    first_idx = np.flatnonzero(streak)
    # At geodetic latitude and longitude zero, ECEF X/Y/Z are up/east/north.
    min_sv = min(int(row["sv"]) for row, ok in zip(rows, used) if ok)
    result.update(
        first_five_within_10m_0p2mps_s=float(times[first_idx[0]]) if len(first_idx) else None,
        epochs_from_first_valid=len(used) - int(np.argmax(used)),
        min_sv_in_valid_fix=min_sv,
        horizontal_p95_m=float(np.percentile(np.linalg.norm(error[:, 1:3], axis=1), 95)),
        position_p95_m=float(np.percentile(position, 95)),
        velocity_p95_mps=float(np.percentile(velocity, 95)),
        mean_up_error_m=float(np.mean(error[:, 0])),
    )
    return result


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("truth", type=Path)
    p.add_argument("receiver", type=Path)
    a = p.parse_args()
    print(json.dumps(summary(table(a.truth), table(a.receiver)), indent=2, allow_nan=False))
