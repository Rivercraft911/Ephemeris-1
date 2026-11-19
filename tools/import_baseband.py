"""Convert an existing interleaved signed 8-bit, 16.368 Msps complex baseband file to EIQ."""

import argparse
import math
import os
from pathlib import Path
import sys
from tempfile import TemporaryDirectory

import numpy as np

from iqio import HEADER, IF_HZ, RATE, SAMPLES_MS
from vectors import pack


def convert(raw, out, count, gain, sigma, seed):
    if count <= 0 or count % SAMPLES_MS:
        raise ValueError("input must contain a positive whole number of milliseconds")
    if not (math.isfinite(gain) and math.isfinite(sigma) and gain > 0 and sigma >= 0):
        raise ValueError("gain must be positive and noise sigma nonnegative")
    rng = np.random.default_rng(seed)
    out.write(HEADER.pack(b"EIQ1", RATE, IF_HZ, 1, 0, count))
    for first in range(0, count, 10 * SAMPLES_MS):
        n = min(10 * SAMPLES_MS, count - first)
        data = raw.read(2 * n)
        if len(data) != 2 * n:
            raise ValueError("short I/Q input")
        pair = np.frombuffer(data, np.int8).reshape(-1, 2)
        z = (gain * (pair[:, 0] + 1j * pair[:, 1])).astype(np.complex64)
        # exp(+j*pi*n/2) maps complex baseband to the board's positive IF.
        z[1::4] *= 1j
        z[2::4] *= -1
        z[3::4] *= -1j
        if sigma:
            z += sigma * (
                rng.standard_normal(n, dtype=np.float32)
                + 1j * rng.standard_normal(n, dtype=np.float32)
            )
        out.write(pack(z))
    if raw.read(1):
        raise ValueError("input exceeds declared length")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path)
    p.add_argument("output", type=Path, help="EIQ file, or - for stdout")
    p.add_argument(
        "--gain", required=True, type=float, help="ADC threshold units per input count"
    )
    p.add_argument(
        "--noise-sigma",
        type=float,
        default=0.0,
        help="optional Gaussian sigma per ADC axis",
    )
    p.add_argument("--seed", type=int, default=10209)
    a = p.parse_args()
    size = a.input.stat().st_size
    if a.output.resolve() == a.input.resolve() or size % (2 * SAMPLES_MS) or not size:
        p.error("use a separate output and complete interleaved 1 ms I/Q blocks")
    with a.input.open("rb") as raw:
        if a.output == Path("-"):
            convert(raw, sys.stdout.buffer, size // 2, a.gain, a.noise_sigma, a.seed)
        else:
            if a.output.exists():
                p.error("output already exists")
            with TemporaryDirectory(dir=a.output.parent) as tmp:
                path = Path(tmp) / "capture.eiq"
                with path.open("xb") as out:
                    convert(raw, out, size // 2, a.gain, a.noise_sigma, a.seed)
                os.link(path, a.output)
