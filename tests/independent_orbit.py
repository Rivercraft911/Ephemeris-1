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


def rate_trial(path, motion, seconds):
    """Compare output decimation and causal position averaging at identical epochs."""
    data = np.genfromtxt(path, delimiter=",", names=True)
    n = seconds * Q["hz"]
    samples = np.arange(1, n + 1) * (FS // Q["hz"])
    if data.size != n or not np.array_equal(data["sample"], samples):
        raise ValueError("expected the complete 5 Hz status stream")
    times = samples / FS
    truth = motion(times)[:, :3]
    pos = np.column_stack([data[k] for k in XYZ[:3]])
    vel = np.column_stack([data[k] for k in XYZ[3:]])
    valid = data["valid"] == 1
    raw = np.linalg.norm(pos - truth, axis=1)
    result = []
    for count in (5, 25, 40):
        errors, indices = [], []
        for i in range(count - 1, n):
            window = slice(i - count + 1, i + 1)
            if not valid[window].all() or data["gaps"][window.start] != data["gaps"][i]:
                continue
            # Under trapezoidal integration the oldest displacement enters one
            # prediction, the next enters two, and the newest enters count-1.
            steps = (vel[window][1:] + vel[window][:-1]) / (2 * Q["hz"])
            current = pos[window].mean(axis=0) + np.arange(1, count) @ steps / count
            indices.append(i)
            errors.append(np.linalg.norm(current - truth[i]))
        idx = np.asarray(indices, dtype=int)
        whole = samples[idx] % FS == 0
        if not whole.any():
            continue
        filtered = np.asarray(errors)
        result.append(dict(samples_per_window=count, span_s=(count - 1) / Q["hz"],
                           epochs_5hz=len(idx), epochs_1hz=int(whole.sum()),
                           raw_5hz_p95_m=float(np.percentile(raw[idx], 95)),
                           filtered_5hz_p95_m=float(np.percentile(filtered, 95)),
                           raw_1hz_p95_m=float(np.percentile(raw[idx[whole]], 95)),
                           filtered_1hz_p95_m=float(np.percentile(filtered[whole], 95))))
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
    opts.add_argument("--rate-trial", type=Path, help="compare 1 Hz and 5 Hz outputs on Pico fix records")
    opts.add_argument("--compare-position", nargs=2, type=Path, metavar=("REFERENCE", "CANDIDATE"),
                      help="compare positions when every valid epoch uses the same satellites")
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
    elif args.rate_trial:
        print(json.dumps(rate_trial(args.rate_trial, motion, args.seconds), indent=2))
    elif args.compare_position:
        result = compare_positions(*args.compare_position, motion, args.seconds)
        print(json.dumps(result, indent=2))
    else:
        result = check(args.score, motion, args.seconds, start_tow_s=525609 + args.offset_s)
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result["passed"] else 1)
