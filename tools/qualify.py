"""Score a complete conducted-RF LEO campaign against the frozen acceptance gates.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
import math
from pathlib import Path

from campaign import Q, SPEC, orbit_period, score, table
from run_campaign import CASES, digest


def qualify(directory):
    entries = []
    errors = []
    for name, (km, index, start) in CASES.items():
        case = directory / name
        try:
            meta = json.loads((case / "run.json").read_text())
            if (
                meta["scenario"] != name
                or meta["level"] != "conducted RF"
                or meta["start_mode"] != "cold"
            ):
                raise ValueError("expected matching unaided cold conducted-RF run")
            for field in (
                "firmware_sha256",
                "scenario_sha256",
                "simulator_version",
                "board_id",
                "timing_reference",
                "rf_calibration",
            ):
                if not isinstance(meta[field], str) or not meta[field].strip():
                    raise ValueError(f"missing {field}")
            for field in ("firmware_sha256", "scenario_sha256"):
                if len(meta[field]) != 64 or any(
                    c not in "0123456789abcdefABCDEF" for c in meta[field]
                ):
                    raise ValueError(f"invalid {field}")
            if (
                meta["altitude_km"] != km
                or meta["inclination_deg"] != SPEC["inclination_deg"]
                or meta["phase_deg"] != start["phase_deg"]
                or meta["gps_week"] != start["gps_week"]
                or meta["tow_s"] != start["tow_s"]
            ):
                raise ValueError("scenario metadata differs from frozen suite")
            truth = table(case / "truth.csv")
            receiver = table(case / "receiver.csv")
            required_duration = math.ceil(orbit_period(km)) if index == 0 else 150
            if len(truth) < required_duration * Q["hz"] + 1:
                raise ValueError(
                    f"expected at least {required_duration} s of independently logged truth"
                )
            result = score(
                truth,
                receiver,
                float(meta["sample_zero_s"]),
                sample_rate_hz=float(meta["sample_rate_hz"]),
                gps_start=(meta["gps_week"], meta["tow_s"]),
            )
            # Independent hardware counters from the final firmware stats report.
            for field in ("missed_blocks", "fifo_stalls", "clock_timeouts"):
                if type(meta[field]) is not int or meta[field] != 0:
                    result["faults"].append(field)
            if (
                not math.isfinite(float(meta["rf_underruns"]))
                or meta["rf_underruns"] != 0
            ):
                result["faults"].append("simulator RF underruns")
            result["passed"] = not result["faults"]
            entries.append(
                dict(
                    id=name,
                    truth_sha256=digest(case / "truth.csv"),
                    receiver_sha256=digest(case / "receiver.csv"),
                    result=result,
                )
            )
        except (OSError, KeyError, ValueError, IndexError) as e:
            errors.append(f"{name}: {e}")
    return {
        "level": "conducted RF",
        "passed": not errors and all(r["result"]["passed"] for r in entries),
        "errors": errors,
        "runs": entries,
    }


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    a = p.parse_args()
    report = qualify(a.directory)
    print(json.dumps(report, indent=2, allow_nan=False))
    raise SystemExit(0 if report["passed"] else 1)
