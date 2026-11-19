"""Inspect the signed IF and level occupancy of a first-board conducted-tone capture.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
from pathlib import Path

import numpy as np

from iqio import IF_HZ, RATE, read_capture
from vectors import unpack


def inspect(packed, expected=IF_HZ, tolerance=10000, min_peak_db=20):
    z = unpack(packed)
    if not len(z):
        raise ValueError("empty tone recording")
    window = np.hanning(len(z))
    spectrum = np.abs(np.fft.fft((z - np.mean(z)) * window)) ** 2
    idx = int(np.argmax(spectrum))
    peak = float(np.fft.fftfreq(len(z), 1 / RATE)[idx])
    floor = max(float(np.median(spectrum)), np.finfo(float).tiny)
    peak_db = 10 * (
        np.log10(max(spectrum[idx], np.finfo(float).tiny)) - np.log10(floor)
    )
    return {
        "samples": len(z),
        "signed_peak_hz": peak,
        "expected_hz": expected,
        "bin_hz": RATE / len(z),
        "peak_over_median_db": float(peak_db),
        "i_level_counts": {
            str(v): int(np.count_nonzero(z.real == v)) for v in (-3, -1, 1, 3)
        },
        "q_level_counts": {
            str(v): int(np.count_nonzero(z.imag == v)) for v in (-3, -1, 1, 3)
        },
        "passed": bool(abs(peak - expected) <= tolerance and peak_db >= min_peak_db),
    }


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("capture", type=Path)
    p.add_argument("--expected-hz", type=float, default=IF_HZ)
    p.add_argument("--tolerance-hz", type=float, default=10000)
    a = p.parse_args()
    with a.capture.open("rb") as f:
        _, packed = read_capture(f)
    result = inspect(packed, a.expected_hz, a.tolerance_hz)
    print(json.dumps(result, indent=2, allow_nan=False))
    raise SystemExit(0 if result["passed"] else 1)
