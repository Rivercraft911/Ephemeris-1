"""Archive, seal and score a physical cold-start GNSS campaign. SPDX-License-Identifier: Apache-2.0"""

import argparse
import csv
import json
import math
from pathlib import Path
import shutil
import sys
import tempfile
import time
import zipfile

import numpy as np

from campaign import (
    EARTH_RADIUS,
    FS,
    Q,
    SPEC,
    SPEC_PATH,
    TRUTH_HEADER,
    circular,
    orbit_period,
    score,
    table,
)
from run_campaign import CASES, digest
from skydel_truth import measurements

SETUP_FIELDS = (
    "rf_board_id",
    "pico_usb_serial",
    "simulator_id",
    "simulator_version",
    "timing_reference",
    "rf_calibration",
)
RUN_FILES = ("simulator.zip", "truth.csv", "receiver.csv", "timing.csv")


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, allow_nan=False) + "\n")


def read_json(path):
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError(f"{path.name} must contain an object")
    return value


def plan(name):
    km, index, start = CASES[name]
    return dict(
        scenario=name,
        altitude_km=km,
        inclination_deg=SPEC["inclination_deg"],
        raan_deg=SPEC["raan_deg"],
        **start,
        duration_s=math.ceil(orbit_period(km)) if index == 0 else 150,
    )


def validate_setup(setup):
    for field in SETUP_FIELDS:
        if not isinstance(setup.get(field), str) or not setup[field].strip():
            raise ValueError(f"missing equipment or calibration field {field}")


def prepare(directory, firmware, setup):
    equipment = read_json(setup)
    validate_setup(equipment)
    if not firmware.is_file() or not firmware.stat().st_size:
        raise ValueError("supply the actual flashed UF2 image")
    directory.parent.mkdir(parents=True, exist_ok=True)
    if directory.exists():
        raise ValueError("choose a new directory for each physical campaign")
    # One rename publishes a complete template; an interrupted copy is retryable.
    with tempfile.TemporaryDirectory(dir=directory.parent) as tmp:
        root = Path(tmp) / "campaign"
        root.mkdir()
        shutil.copyfile(firmware, root / "firmware.uf2")
        shutil.copyfile(SPEC_PATH, root / "scenarios.json")
        meta = dict(
            format=1,
            level="conducted RF",
            equipment=equipment,
            firmware_sha256=digest(root / "firmware.uf2"),
            suite_sha256=digest(SPEC_PATH),
        )
        write_json(root / "campaign.json", meta)
        campaign_sha = digest(root / "campaign.json")
        for name in CASES:
            case = root / name
            case.mkdir()
            write_json(
                case / "run.json",
                {
                    **plan(name),
                    "start_mode": "cold",
                    "campaign_sha256": campaign_sha,
                    "evidence_sha256": {},
                    "rf_underruns": None,
                },
            )
        root.rename(directory)


def inventory(directory):
    meta = read_json(directory / "campaign.json")
    if meta["format"] != 1 or meta["level"] != "conducted RF":
        raise ValueError("expected a version 1 conducted RF manifest")
    validate_setup(meta["equipment"])
    if (
        digest(directory / "firmware.uf2") != meta["firmware_sha256"]
        or digest(directory / "scenarios.json") != meta["suite_sha256"]
        or digest(SPEC_PATH) != meta["suite_sha256"]
    ):
        raise ValueError("archived firmware or frozen scenario hash differs")
    return meta


def check_identity(directory, name, record):
    if record["start_mode"] != "cold" or record["campaign_sha256"] != digest(
        directory / "campaign.json"
    ):
        raise ValueError("cold-start or campaign identity differs")
    if any(record[key] != expected for key, expected in plan(name).items()):
        raise ValueError("scenario parameters differ from the frozen suite")


def seal(directory, name, rf_underruns):
    inventory(directory)
    case = directory / name
    record = read_json(case / "run.json")
    check_identity(directory, name, record)
    if record["evidence_sha256"]:
        raise ValueError("run already sealed")
    if type(rf_underruns) is not int or rf_underruns < 0:
        raise ValueError("supply the observed nonnegative simulator RF underrun count")
    for filename in RUN_FILES:
        if not (case / filename).stat().st_size:
            raise ValueError(f"empty evidence file {filename}")
    with zipfile.ZipFile(case / "simulator.zip") as archive:
        if not any(info.file_size for info in archive.infolist()):
            raise ValueError("simulator archive is empty")
    record["evidence_sha256"] = {
        filename: digest(case / filename) for filename in RUN_FILES
    }
    record["rf_underruns"] = rf_underruns
    with tempfile.NamedTemporaryFile(mode="w", dir=case, delete=False) as f:
        json.dump(record, f, indent=2)
        f.write("\n")
    Path(f.name).replace(case / "run.json")


def timebase(rows):
    # Reference times include the independently calibrated fixed RF and trigger delay.
    if len(rows) < 2:
        raise ValueError("two clock observations required")
    samples = [int(r["sample"]) for r in rows]
    seconds = [float(r["elapsed_s"]) for r in rows]
    if (
        samples[0] < 0
        or not all(math.isfinite(t) for t in seconds)
        or any(b <= a for a, b in zip(seconds, seconds[1:]))
        or any(b <= a for a, b in zip(samples, samples[1:]))
    ):
        raise ValueError("timing observations must be finite and strictly increasing")
    rate = (samples[-1] - samples[0]) / (seconds[-1] - seconds[0])
    offset = seconds[0] - samples[0] / rate
    if not (0.99 * FS <= rate <= 1.01 * FS and 0 <= offset < seconds[-1]):
        raise ValueError("check measured ADC rate and sample-zero time from power-on")
    residual = max(abs(t - offset - n / rate) for t, n in zip(seconds, samples))
    return offset, rate, residual


def hardware_stats(path, last_sample, expected_serial):
    serials = []
    stats = None
    with path.open() as transcript:
        for line in transcript:
            if line.startswith("#usb_serial="):
                serials.append(line.strip().split("=", 1)[1])
            if line.startswith("#stats "):
                stats = {k: int(v) for k, v in (s.split("=") for s in line.split()[1:])}
    if serials != [expected_serial]:
        raise ValueError("attached Pico USB serial differs from campaign equipment")
    if stats is None:
        raise ValueError("missing final Pico #stats in CDC transcript")
    if stats["blocks"] * (FS // 1000) < last_sample:
        raise ValueError("final Pico #stats precedes the last receiver epoch")
    faults = [
        k
        for k in (
            "missed",
            "fifo_stalls",
            "clock_timeouts",
            "dropped_usb_lines",
            "warm_restored",
        )
        if stats[k] != 0
    ]
    if stats["clock_alive"] != 1:
        faults.append("clock_alive")
    return stats, faults


def run_result(directory, name, equipment):
    case = directory / name
    record = read_json(case / "run.json")
    check_identity(directory, name, record)
    hashes = {p: digest(case / p) for p in RUN_FILES}
    if hashes != record["evidence_sha256"]:
        raise ValueError("files changed or run has not been sealed")
    count = record["rf_underruns"]
    if type(count) is not int or count < 0:
        raise ValueError("invalid simulator underrun count")
    truth, rx = table(case / "truth.csv"), table(case / "receiver.csv")
    if len(truth) != record["duration_s"] * Q["hz"] + 1 or not rx:
        raise ValueError(
            f"expected {record['duration_s']} s of 5 Hz truth and a receiver transcript"
        )
    # Check the simulator's own timestamps before using them as a truth reference.
    for row in truth:
        tow = record["tow_s"] + float(row["t_s"])
        if int(row["gps_week"]) != record["gps_week"] + int(
            tow // 604800
        ) or not math.isclose(
            float(row["tow_s"]), tow % 604800, rel_tol=0, abs_tol=1e-5
        ):
            raise ValueError("simulator GPS epoch differs from frozen power-on epoch")
    # Distinguish all three altitude bands and four start phases even if the
    # operator selected the wrong motion profile at the correct GPS epoch.
    t = np.array([float(row["t_s"]) for row in truth])
    expected = circular(
        EARTH_RADIUS + 1000 * record["altitude_km"],
        math.radians(record["inclination_deg"]),
        math.radians(record["raan_deg"]),
        math.radians(record["phase_deg"]),
        t,
    )
    positions = np.array(
        [[float(row[k]) for k in ("x_m", "y_m", "z_m")] for row in truth]
    )
    half_spacing_m = 500 * min(np.diff(sorted(SPEC["altitudes_km"])))
    if np.any(np.linalg.norm(positions - expected[:, :3], axis=1) >= half_spacing_m):
        raise ValueError("simulator trajectory differs from the declared orbital case")
    exported = np.array(
        [[float(row[k]) for k in (*TRUTH_HEADER, "gps_week", "tow_s")] for row in truth]
    )
    raw_truth = measurements(case / "simulator.zip", record)
    if not np.allclose(raw_truth, exported, rtol=1e-12, atol=1e-9):
        raise ValueError("truth.csv differs from the archived simulator raw logs")
    markers = table(case / "timing.csv")
    zero, rate, residual = timebase(markers)
    result = score(
        truth,
        rx,
        zero,
        sample_rate_hz=rate,
        gps_start=(record["gps_week"], record["tow_s"]),
    )
    stats, counters = hardware_stats(
        case / "receiver.csv", int(rx[-1]["sample"]), equipment["pico_usb_serial"]
    )
    faults = result["faults"]
    faults.extend(f"Pico {f}" for f in counters)
    if count:
        faults.append("simulator RF underruns")
    result["passed"] = not faults
    return dict(
        id=name,
        run_sha256=digest(case / "run.json"),
        evidence_sha256=hashes,
        sample_zero_s=zero,
        sample_rate_hz=rate,
        max_marker_residual_s=residual,
        pico_stats=stats,
        rf_underruns=count,
        result=result,
    )


def qualify(directory):
    entries, errors = [], []
    campaign = None
    try:
        campaign = inventory(directory)
    except (OSError, KeyError, TypeError, ValueError) as exc:
        errors.append(f"campaign: {exc}")
    if campaign:
        for name in CASES:
            try:
                entries.append(run_result(directory, name, campaign["equipment"]))
            except (
                OSError,
                KeyError,
                IndexError,
                TypeError,
                ValueError,
                csv.Error,
            ) as exc:
                errors.append(f"{name}: {exc}")
    return dict(
        level="conducted RF",
        campaign=campaign,
        passed=not errors
        and len(entries) == len(CASES)
        and all(r["result"]["passed"] for r in entries),
        errors=errors,
        runs=entries,
    )


def collect(directory, name):
    import serial
    from serial.tools import list_ports

    case = directory / name
    record = read_json(case / "run.json")
    check_identity(directory, name, record)
    hw = inventory(directory)["equipment"]["pico_usb_serial"]
    path = case / "receiver.csv"
    partial = path.with_suffix(".partial")
    if path.exists() or partial.exists():
        raise ValueError(f"preserve or move the existing receiver transcript in {case}")
    print(
        f"Waiting for Pico USB serial {hw}. Apply receiver power on the simulator event.",
        flush=True,
    )
    while True:
        port = next(
            (p.device for p in list_ports.comports() if p.serial_number == hw), None
        )
        if port:
            break
        time.sleep(0.1)
    with serial.Serial(
        port, 115200, timeout=0.5, write_timeout=1
    ) as device, partial.open("xb") as log:
        log.write(f"#usb_serial={hw}\n".encode("ascii"))
        sample = 0
        fields = []
        deadline = time.monotonic() + record["duration_s"] + 30
        next_request = 0.0
        while time.monotonic() < deadline:
            raw = device.readline()
            log.write(raw)
            text = raw.decode("ascii").strip()
            if text.startswith("sequence,sample,"):
                fields = text.split(",")
            elif text.startswith("#stats ") and next_request > 0:
                break
            elif text and fields and text[0].isdigit():
                row = dict(zip(fields, text.split(",")))
                sample = int(row["sample"])
            if sample >= record["duration_s"] * FS and time.monotonic() >= next_request:
                device.write(b"stats\n")
                next_request = time.monotonic() + 0.5
        else:
            raise ValueError(f"capture ended without final #stats; retained {partial}")
    partial.rename(path)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    args = sys.argv[1:]
    # Retain 'qualify.py DIRECTORY' for existing scoring scripts.
    if args and args[0] not in (
        "prepare",
        "seal",
        "score",
        "collect",
        "truth",
        "-h",
        "--help",
    ):
        args = ["score", *args]
    commands = cli.add_subparsers(dest="action", required=True)
    for action in ("prepare", "seal", "score", "collect", "truth"):
        parser = commands.add_parser(action)
        parser.add_argument("directory", type=Path)
        if action == "prepare":
            parser.add_argument("--firmware", type=Path, required=True)
            parser.add_argument("--setup", type=Path, required=True)
        if action in ("seal", "collect", "truth"):
            parser.add_argument("scenario", choices=list(CASES))
        if action == "seal":
            parser.add_argument("--rf-underruns", type=int, required=True)
    a = cli.parse_args(args)
    try:
        if a.action == "prepare":
            prepare(a.directory, a.firmware, a.setup)
        elif a.action == "seal":
            seal(a.directory, a.scenario, a.rf_underruns)
        elif a.action == "truth":
            from skydel_truth import convert

            case = a.directory / a.scenario
            record = read_json(case / "run.json")
            check_identity(a.directory, a.scenario, record)
            convert(case / "simulator.zip", case / "truth.csv", record)
        elif a.action == "collect":
            collect(a.directory, a.scenario)
        else:
            result = qualify(a.directory)
            print(json.dumps(result, indent=2, allow_nan=False))
            return int(not result["passed"])
    except (OSError, KeyError, TypeError, ValueError, zipfile.BadZipFile) as exc:
        cli.exit(1, str(exc) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
