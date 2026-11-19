"""Import the CTTC 4 MS/s little-endian signed-16 I/Q recording into EIQ1.

The fixed ADC magnitude threshold is the joint per-axis RMS of the first
100 ms of source samples. No position, navigation bits or noise enter the conversion.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import os
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np
from scipy.signal import firwin, resample_poly

from iqio import HEADER, IF_HZ, RATE, SAMPLES_MS
from vectors import pack

INPUT_MS = 4000
UP, DOWN = 1023, 250  # 16.368 MHz / 4 MHz


def convert(iq, out, block_ms=32):
    count = len(iq)
    if not count or count % INPUT_MS or block_ms < 1:
        raise ValueError("expected complete, nonempty milliseconds of interleaved I/Q")
    calibration = iq[:100 * INPUT_MS].astype(np.float64)
    threshold = np.sqrt(np.mean(calibration * calibration))
    if threshold == 0:
        raise ValueError("no signal power in the calibration interval")

    # A centered low-pass FIR with 2 MHz cutoff on the upsampled grid.
    # Its support extends only 10 source samples each way. Read one whole
    # 250-sample phase period on either side so each chunk shares the same
    # time origin as a single resampling pass over the whole recording.
    taps = firwin(20 * UP + 1, 1 / UP, window=("kaiser", 5.0)).astype(np.float32)
    out.write(HEADER.pack(b"EIQ1", RATE, IF_HZ, 1, 0, count // INPUT_MS * SAMPLES_MS))
    stride = block_ms * INPUT_MS
    for start in range(0, count, stride):
        end = min(start + stride, count)
        left, right = max(0, start - DOWN), min(end + DOWN, count)
        source = iq[left:right].astype(np.float32).view(np.complex64).ravel()
        filtered = resample_poly(source, UP, DOWN, window=taps)
        first = (start - left) // DOWN * UP
        samples = filtered[first:first + (end - start) // DOWN * UP]
        samples /= np.float32(threshold)
        # Every whole millisecond begins at the same quarter-IF phase.
        samples[1::4] *= 1j
        samples[2::4] *= -1
        samples[3::4] *= -1j
        out.write(pack(samples))
    return float(threshold)


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("input", type=Path, help="CTTC interleaved little-endian I16 Q16 baseband")
    cli.add_argument("output", type=Path, help="new EIQ1 file")
    args = cli.parse_args()
    if args.output.exists():
        cli.error("output already exists")
    size = args.input.stat().st_size
    if not size or size % (4 * INPUT_MS):
        cli.error("capture must contain complete 4 MS/s milliseconds")
    iq = np.memmap(args.input, mode="r", dtype="<i2", shape=(size // 4, 2))
    with TemporaryDirectory(dir=args.output.parent) as tmp:
        path = Path(tmp) / "capture.eiq"
        with path.open("xb") as out:
            rms = convert(iq, out)
        os.link(path, args.output)
    print(f"{len(iq) // INPUT_MS} ms, ADC magnitude threshold {rms:.9g} input counts")
