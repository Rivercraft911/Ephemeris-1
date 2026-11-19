"""Convert published Tampere and FGI GPS L1 recordings to EIQ1.

The first stage produces centered 4 MS/s complex baseband. The second uses the
same IF, quantization and sample convention as the CTTC adapter.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
from fractions import Fraction
from math import gcd
import os
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np
from scipy.signal import firwin, resample_poly

from import_cttc import INPUT_MS, convert

# Input samples per second and digital IF in Hz. An IF of zero denotes I8,Q8.
FORMATS = {"tampere21": (10_000_000, 0), "tampere24": (40_000_000, 10_000_000),
           "fgi": (26_000_000, 6_390_000)}


def baseband(source, rate, if_hz, duration_ms, output, block_ms=64):
    """Mix and decimate complete milliseconds, preserving carrier phase across blocks."""
    ratio = Fraction(INPUT_MS * 1000, rate)
    up, down = ratio.numerator, ratio.denominator
    taps = firwin(20 * down + 1, 1 / down, window=("kaiser", 5.0)).astype(np.float32)
    # All supported ratios have down >= up. Pad by exactly the centered FIR's
    # support, rounded to a full input phase period for identical chunk alignment.
    pad = ((len(taps) // 2 + up * down - 1) // (up * down)) * down
    samples_ms = rate // 1000
    count = duration_ms * samples_ms
    if if_hz:
        period = rate // gcd(rate, if_hz)
        lo = np.exp(-2j * np.pi * (np.arange(period) * if_hz % rate) / rate).astype(np.complex64)
    for start in range(0, count, block_ms * samples_ms):
        end = min(start + block_ms * samples_ms, count)
        left, right = max(start - pad, 0), min(end + pad, count)
        raw = source[left:right].astype(np.float32)
        if if_hz:
            # The real input contains both sidebands. The FIR below removes the image.
            mixed = raw * lo[np.arange(left, right) % period]
        else:
            mixed = raw.view(np.complex64).ravel()
        filtered = resample_poly(mixed, up, down, window=taps)
        offset = (start - left) * up // down
        n = (end - start) * up // down
        output[start * up // down:end * up // down] = filtered[offset:offset + n]


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('format', choices=FORMATS)
    cli.add_argument('input', type=Path)
    cli.add_argument('output', type=Path)
    cli.add_argument('--milliseconds', type=int, help='use only this prefix of the recording')
    args = cli.parse_args()
    if args.output.exists():
        cli.error('output already exists')
    rate, if_hz = FORMATS[args.format]
    channels = 1 if if_hz else 2
    size = args.input.stat().st_size
    if size % channels:
        cli.error('input ends in an incomplete I/Q pair')
    complete_ms = size // (channels * (rate // 1000))
    duration = args.milliseconds if args.milliseconds is not None else complete_ms
    if not 0 < duration <= complete_ms:
        cli.error('duration must fit within the complete milliseconds in the file')
    shape = (size,) if if_hz else (size // 2, 2)
    source = np.memmap(args.input, dtype=np.int8, mode='r', shape=shape)
    with TemporaryDirectory(dir=args.output.parent) as tmp:
        iq = np.memmap(Path(tmp) / 'baseband', mode='w+', dtype=np.complex64,
                       shape=(duration * INPUT_MS,))
        baseband(source, rate, if_hz, duration, iq)
        path = Path(tmp) / 'capture.eiq'
        with path.open('xb') as out:
            rms = convert(iq.view(np.float32).reshape(-1, 2), out)
        os.link(path, args.output)
    print(f'{duration} ms, ADC magnitude threshold {rms:.9g} baseband counts')


if __name__ == '__main__':
    main()
