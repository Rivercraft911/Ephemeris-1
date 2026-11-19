"""Compare Ephemeris fixes with a 10 Hz ground receiver reference in GPS time.

The reference CSV contains gps_week,tow_s,x_m,y_m,z_m. Positions and
heights use WGS 84. All comparisons describe receiver-to-receiver separation.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import csv
import gzip
import json
import math
from pathlib import Path

import numpy as np

from ground_fix import geodetic
from iqio import RATE

MAX_INTERVAL_S = 0.15  # A missing nominal 10 Hz reference fix leaves a 0.2 s gap.


def rows(path):
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", newline="") as f:
        return list(csv.DictReader(f))


def position(row):
    xyz = np.array([float(row[k]) for k in ("x_m", "y_m", "z_m")])
    if not np.isfinite(xyz).all() or not xyz.any():
        raise ValueError("invalid ECEF position")
    return xyz


def tow(row):
    value = float(row["tow_s"])
    if not 0 <= value < 604800:
        raise ValueError("invalid GPS time of week")
    return value


def llh(xyz):
    return dict(zip(("lat_deg", "lon_deg", "ellipsoid_height_m"), geodetic(*xyz)))


def reference_tracks(reference):
    weeks = {}
    previous = (-1, 0)
    for row in reference:
        week, t = int(row["gps_week"]), tow(row)
        if week < 0 or (week, t) <= previous:
            raise ValueError("reference GPS times must strictly increase")
        weeks.setdefault(week, []).append((t, *position(row)))
        previous = week, t
    tracks = {w % 1024: np.array(records) for w, records in weeks.items()}
    if len(tracks) != len(weeks):
        raise ValueError("reference contains ambiguous 10-bit GPS weeks")
    return tracks


def match(track, t):
    if track is None:
        return None
    times = track[:, 0]
    right = np.searchsorted(times, t)
    if right < len(times) and times[right] == t:
        return track[right, 1:]
    if right == 0 or right == len(times):
        return None
    left = right - 1
    interval = times[right] - times[left]
    if interval > MAX_INTERVAL_S:
        return None
    a, b = track[left, 1:], track[right, 1:]
    return a + (t - times[left]) / interval * (b - a)


def separation(xyz, reference):
    lat_deg, lon_deg, _ = geodetic(*reference)
    lat, lon = math.radians(lat_deg), math.radians(lon_deg)
    dx, dy, dz = xyz - reference
    in_plane = math.cos(lon) * dx + math.sin(lon) * dy
    east = -math.sin(lon) * dx + math.cos(lon) * dy
    north = -math.sin(lat) * in_plane + math.cos(lat) * dz
    up = math.cos(lat) * in_plane + math.sin(lat) * dz
    return math.hypot(east, north), abs(up), math.hypot(dx, dy, dz)


def compare(receiver, reference):
    tracks = reference_tracks(reference)
    fixes, points = [], []
    first_fix = None
    for row in receiver:
        if row["valid"] == "0":
            continue
        if row["valid"] != "1":
            raise ValueError("invalid receiver validity flag")
        xyz, week, t = position(row), int(row["week_mod1024"]), tow(row)
        if not 0 <= week < 1024:
            raise ValueError("invalid 10-bit receiver GPS week")
        elapsed = int(row["sample"]) / RATE
        if first_fix is None:
            first_fix = {"elapsed_s": elapsed, "week_mod1024": week, "tow_s": t, **llh(xyz)}
        fixes.append(xyz)
        ref = match(tracks.get(week), t)
        if ref is not None:
            points.append((elapsed, *separation(xyz, ref)))

    metrics = {}
    if points:
        for key, values in zip(("horizontal_m", "abs_vertical_m", "distance_3d_m"),
                               np.asarray(points)[:, 1:].T):
            metrics[key] = {"median": float(np.median(values)), "p95": float(np.percentile(values, 95))}
    return {
        "receiver_rows": len(receiver), "valid_fixes": len(fixes), "first_fix": first_fix,
        "llh_of_componentwise_median_ecef": llh(np.median(fixes, axis=0)) if fixes else None,
        "reference_rows": len(reference), "reference_max_interpolation_interval_s": MAX_INTERVAL_S,
        "matched_fixes": len(points), "separation": metrics,
    }, points


def plot(points, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    if not points:
        raise ValueError("plot requires at least one time-matched fix")
    values = np.asarray(points)
    with plt.style.context("dark_background"):
        fig, ax = plt.subplots(figsize=(8, 3.6), layout="constrained")
        for column, label, color in ((3, "3D", "#dedede"), (1, "Horizontal", "#2fa8ff"),
                                     (2, "Absolute vertical", "#ff8c3a")):
            ax.scatter(values[:, 0], values[:, column], s=7, linewidths=0, color=color, label=label)
        ax.set(xlabel="Seconds from capture start", ylabel="Position separation (m)",
               title="Ephemeris 1 and GNSS-SDR on recorded ground I/Q", xlim=(0, None), ylim=(0, None))
        ax.legend(frameon=False, ncol=3)
        ax.grid(axis="y", alpha=0.25)
        fig.savefig(path, dpi=180)
        plt.close(fig)


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("receiver", type=Path, help="Ephemeris status CSV or CSV.gz")
    cli.add_argument("reference", type=Path, help="GNSS-SDR GPS time and ECEF CSV or CSV.gz")
    cli.add_argument("--plot", type=Path, help="save a scatter plot of matched fixes")
    args = cli.parse_args()
    summary, points = compare(rows(args.receiver), rows(args.reference))
    print(json.dumps(summary, indent=2, allow_nan=False))
    if args.plot:
        plot(points, args.plot)
