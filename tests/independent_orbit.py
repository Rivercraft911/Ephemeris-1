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


def read_fixes(path, seconds):
    data = np.atleast_1d(np.genfromtxt(path, delimiter=",", names=True))
    n = seconds * Q["hz"]
    expected = np.arange(1, n + 1) * (FS // Q["hz"])
    if data.size != n or not np.array_equal(data["sample"], expected):
        raise ValueError("expected the complete 5 Hz status stream")
    gaps = data["gaps"]
    if (not np.isin(data["valid"], [0, 1]).all() or not np.isfinite(gaps).all()
            or np.any(gaps < 0) or np.any(gaps != np.floor(gaps)) or np.any(np.diff(gaps) < 0)):
        raise ValueError("invalid fix flag or gap counter")
    valid = data["valid"] == 1
    # Paired firmware reports its filtered position in x_m/y_m/z_m. Use its
    # independent raw columns as the input whenever they are present.
    raw_keys = tuple("raw_" + k for k in XYZ[:3])
    if any(k in data.dtype.names for k in raw_keys) and not all(k in data.dtype.names for k in raw_keys):
        raise ValueError("incomplete raw position columns")
    keys = raw_keys if raw_keys[0] in data.dtype.names else XYZ[:3]
    raw = np.column_stack([data[k] for k in keys])
    vel = np.column_stack([data[k] for k in XYZ[3:]])
    if not np.isfinite(raw[valid]).all() or not np.isfinite(vel[valid]).all():
        raise ValueError("nonfinite valid solution")
    return data, valid, raw, vel


def motion_average(raw, vel, valid, gaps, count):
    """Direct weighted trapezoidal integration from the raw window at each fix."""
    if count < 1:
        raise ValueError("window must contain at least one fix")
    reference = np.full_like(raw, np.nan)
    filled = np.zeros(len(raw), dtype=int)
    for i in np.flatnonzero(valid):
        n = 1
        if i and gaps[i] == gaps[i-1]:
            n = min(count, filled[i-1] + 1)
        filled[i] = n
        window = slice(i - n + 1, i + 1)
        # The oldest displacement enters one prediction, the next enters
        # two, and the newest enters n-1. Rebuild the window from scratch.
        v = vel[window]
        steps = (v[1:] + v[:-1]) / (2 * Q["hz"])
        reference[i] = raw[window].mean(axis=0) + np.arange(1, n) @ steps / n
    return reference, filled


def rate_trial(path, motion, seconds):
    """Compare output decimation and causal position averaging at identical epochs."""
    data, valid, pos, vel = read_fixes(path, seconds)
    truth = motion(data["sample"] / FS)[:, :3]
    raw = np.linalg.norm(pos - truth, axis=1)
    result = []
    for count in (5, 25, 40):
        reference, filled = motion_average(pos, vel, valid, data["gaps"], count)
        mature = filled == count
        whole = mature & (data["sample"] % FS == 0)
        if not whole.any():
            continue
        filtered = np.linalg.norm(reference - truth, axis=1)
        result.append(dict(samples_per_window=count, span_s=(count - 1) / Q["hz"],
                           epochs_5hz=int(mature.sum()), epochs_1hz=int(whole.sum()),
                           raw_5hz_p95_m=float(np.percentile(raw[mature], 95)),
                           filtered_5hz_p95_m=float(np.percentile(filtered[mature], 95)),
                           raw_1hz_p95_m=float(np.percentile(raw[whole], 95)),
                           filtered_1hz_p95_m=float(np.percentile(filtered[whole], 95))))
    return result


def score_smoothing(path, motion, seconds, count=40):
    """Score raw and device-filtered positions at the same epochs against truth."""
    data, valid, raw, vel = read_fixes(path, seconds)
    if "raw_x_m" not in data.dtype.names or "motion_samples" not in data.dtype.names:
        raise ValueError("expected paired raw position and motion_samples columns")
    if not valid.any():
        raise ValueError("no valid paired position")
    filtered = np.column_stack([data[k] for k in XYZ[:3]])
    if not np.isfinite(filtered[valid]).all():
        raise ValueError("nonfinite onboard position")
    reference, filled = motion_average(raw, vel, valid, data["gaps"], count)
    if not np.array_equal(filled, data["motion_samples"]):
        raise ValueError("onboard motion_samples differs from the contiguous valid window")
    diff = filtered - reference
    first = np.flatnonzero(valid)[0]
    t = data["sample"] / FS
    truth = motion(t)[:, :3]
    raw_error = np.linalg.norm(raw - truth, axis=1)
    filtered_error = np.linalg.norm(filtered - truth, axis=1)
    result = dict(status_epochs=len(data), window_fixes=count, window_span_s=(count-1)/Q["hz"],
                  first_fix_s=float(t[first]), first_raw_error_m=float(raw_error[first]),
                  first_onboard_error_m=float(filtered_error[first]))
    for label, mask in (("all_valid", valid), ("full_window", filled == count)):
        stats = {"epochs": int(mask.sum())}
        if mask.any():
            stats.update(first_s=float(t[mask][0]),
                         raw_p95_m=float(np.percentile(raw_error[mask], 95)),
                         onboard_p95_m=float(np.percentile(filtered_error[mask], 95)))
        result[label] = stats
    # Both positions are rounded to 1 mm and each velocity component to
    # 10 µm/s on USB. The mean propagation time is half the filled window span.
    bounds = 0.001 + 0.000005 * (filled[valid] - 1) / (2 * Q["hz"])
    result["max_reference_difference_3d_m"] = float(np.linalg.norm(diff[valid], axis=1).max())
    result["max_reference_difference_axis_m"] = float(np.abs(diff[valid]).max())
    result["csv_rounding_bound_axis_m"] = float(bounds.max())
    result["matches_reference"] = bool(np.all(np.abs(diff[valid]) <= bounds[:, None] + 1e-8))
    return result


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
    opts.add_argument("--score-smoothing", type=Path, help="score the paired raw and filtered positions from one replay")
    opts.add_argument("--rate-trial", type=Path, help="compare 1 Hz and 5 Hz outputs on Pico fix records")
    opts.add_argument("--compare-position", nargs=2, type=Path, metavar=("REFERENCE", "CANDIDATE"),
                      help="compare positions when every valid epoch uses the same satellites")
    cli.add_argument("--window-fixes", type=int, default=40, help="expected smoother window length for --score-smoothing")
    cli.add_argument("--seconds", type=int, default=90, help="recording duration")
    cli.add_argument("--offset-s", type=int, default=0, help="seconds discarded before a cold start on this recording")
    cli.add_argument("--altitude-km", type=float, default=550, help="circular orbit altitude")
    cli.add_argument("--phase-deg", type=float, default=12, help="initial argument of latitude")
    args = cli.parse_args()
    if args.seconds <= 0 or args.offset_s < 0:
        cli.error("recording duration must be positive and offset nonnegative")
    if not np.isfinite(args.altitude_km) or args.altitude_km <= 0 or not np.isfinite(args.phase_deg):
        cli.error("use a finite phase and a positive finite orbit altitude")

    def motion(t):
        return trajectory(t + args.offset_s, args.altitude_km, args.phase_deg)

    if args.motion:
        t = np.arange(10 * args.seconds + 1) / 10
        with args.motion.open("x") as f:
            np.savetxt(
                f, np.column_stack((t, motion(t)[:, :3])), fmt="%.9f", delimiter=","
            )
    elif args.score_smoothing:
        result = score_smoothing(args.score_smoothing, motion, args.seconds, args.window_fixes)
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result["matches_reference"] else 1)
    elif args.rate_trial:
        print(json.dumps(rate_trial(args.rate_trial, motion, args.seconds), indent=2))
    elif args.compare_position:
        result = compare_positions(*args.compare_position, motion, args.seconds)
        print(json.dumps(result, indent=2))
    else:
        result = check(args.score, motion, args.seconds, start_tow_s=525609 + args.offset_s)
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result["passed"] else 1)
