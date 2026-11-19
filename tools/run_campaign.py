"""Stream held-out orbital IQ through the native receiver and keep separate truth/evidence.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import subprocess
import sys
import time

from campaign import FS, Q, SPEC, SPEC_PATH, orbit_period, score, table
from stress import Stress

ROOT = SPEC_PATH.parents[1]
CASES = {
    f"leo-{km}-{i+1}": (km, i, s)
    for km in SPEC["altitudes_km"]
    for i, s in enumerate(SPEC["starts"])
}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def stress_summary(truth, rx, config, rate):
    """Score availability of fresh fixes and time to recovery across an intentional RF fault."""
    trow = {round(float(r["t_s"]) * Q["hz"]): r for r in truth}
    before = during = after = 0
    invalid_seen = False
    first_recovery = None
    stale = []
    begin = config.get("start_s", 0)
    end = config.get("end_s", begin)
    for row in rx:
        t = int(row["sample"]) / rate
        tr = trow.get(round(t * Q["hz"]))
        if not tr:
            continue
        valid = row["valid"] == "1"
        if valid:
            dp = math.dist(
                [float(row[k]) for k in ("x_m", "y_m", "z_m")],
                [float(tr[k]) for k in ("x_m", "y_m", "z_m")],
            )
            if not math.isfinite(dp):
                stale.append(t)
        if t < begin:
            before += valid
        elif t < end:
            during += valid
            invalid_seen |= not valid
            if (
                valid
                and config["type"] in ("disable_all_sv", "limit_healthy_sv")
                and t > begin + 1
                and float(tr["healthy_sv"]) < 4
            ):
                stale.append(t)
        elif valid and dp <= Q["max_p95_position_m"]:
            after += 1
            if first_recovery is None:
                first_recovery = t - end
    loss = config["type"] in ("disable_all_sv", "limit_healthy_sv")
    return {
        "pre_fault_valid": before,
        "fault_valid": during,
        "post_fault_truth_checked": after,
        "invalid_seen": invalid_seen,
        "recovery_s": first_recovery,
        "false_valid_epochs": stale,
        "passed": bool(before and after and not stale and (invalid_seen or not loss)),
    }


def run(replay, outdir, name, seconds, work, stress="nominal"):
    km, index, start = CASES[name]
    dest = outdir / name / stress
    dest.mkdir(parents=True, exist_ok=True)
    truth = dest / "truth.csv"
    output = dest / "receiver.csv"
    generate = [
        sys.executable,
        str(ROOT / "tools/sky_signal.py"),
        "-",
        "--seconds",
        str(seconds),
        "--scenario",
        name,
        "--truth",
        str(truth),
        "--seed",
        str(711 + 181 * index + km),
    ]
    if stress != "nominal":
        generate += ["--stress", stress]
    decode = [str(replay.resolve()), "-", "--work", str(work)]
    t0 = time.monotonic()
    with (dest / "generator.log").open("wb") as gen_log, (dest / "replay.log").open(
        "wb"
    ) as rx_log, output.open("wb") as solution:
        gen = subprocess.Popen(generate, stdout=subprocess.PIPE, stderr=gen_log)
        try:
            result = subprocess.run(
                decode, stdin=gen.stdout, stdout=solution, stderr=rx_log
            )
        finally:
            gen.stdout.close()
            gen_rc = gen.wait()
        if gen_rc or result.returncode:
            raise RuntimeError(
                f"generator={gen_rc}, replay={result.returncode}; see {dest}"
            )
    tr, rx = table(truth), table(output)
    rate = FS * Stress(stress).clock_scale
    case = next((c for c in SPEC["stress"] if c["id"] == stress), None)
    if case and "end_s" in case:
        scores = stress_summary(tr, rx, case, rate)
    else:
        scores = score(
            tr,
            rx,
            0,
            sample_rate_hz=rate,
            gps_start=(start["gps_week"], start["tow_s"]),
        )
    revision = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True, capture_output=True
    ).stdout.strip()
    dirty = bool(
        subprocess.run(
            ["git", "-C", str(ROOT), "status", "--porcelain"], capture_output=True
        ).stdout
    )
    evidence = {
        "level": "native raw I/Q replay",
        "scenario": name,
        "stress": stress,
        "seconds": seconds,
        "revision": revision,
        "dirty": dirty,
        "host": platform.platform(),
        "runtime_s": time.monotonic() - t0,
        "generator": generate,
        "decoder": decode,
        "receiver_sha256": digest(replay),
        "suite_sha256": digest(SPEC_PATH),
        "truth_sha256": digest(truth),
        "output_sha256": digest(output),
        "result": scores,
    }
    (dest / "report.json").write_text(
        json.dumps(evidence, indent=2, allow_nan=False) + "\n"
    )
    print(name, stress, json.dumps(scores, allow_nan=False), flush=True)
    return scores["passed"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--scenario", choices=["all", *CASES], default="leo-550-1")
    parser.add_argument(
        "--full-orbits",
        action="store_true",
        help="extend start 1 at each altitude to its orbital period",
    )
    parser.add_argument("--seconds", type=int, help="diagnostic subset duration")
    parser.add_argument(
        "--work",
        type=int,
        default=8,
        help="search work items per replay ms; native scheduling knob",
    )
    parser.add_argument(
        "--stress",
        default="nominal",
        choices=[
            "nominal",
            *[s["id"] for s in SPEC["stress"] if s["id"] != "warm-restart"],
        ],
    )
    a = parser.parse_args()
    if a.seconds is not None and a.seconds < 2:
        parser.error("use at least two seconds")
    passed = True
    for name in CASES if a.scenario == "all" else [a.scenario]:
        km, i, _ = CASES[name]
        duration = a.seconds or (
            math.ceil(orbit_period(km)) if a.full_orbits and i == 0 else 150
        )
        if a.stress != "nominal" and not a.seconds:
            duration = max(260, duration)
        passed = run(a.replay, a.out, name, duration, a.work, a.stress) and passed
    raise SystemExit(0 if passed else 1)
