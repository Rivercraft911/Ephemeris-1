"""Independent GPS L1 C/A test source using the ICD G2 *delay* table.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
from pathlib import Path

import numpy as np

from iqio import IF_HZ, RATE, SAMPLES_MS, write_capture

# IS-GPS-200N Table 3-Ia. Production code may instead use the G2 phase-select taps.
DELAYS = (
    5,
    6,
    7,
    8,
    17,
    18,
    139,
    140,
    141,
    251,
    252,
    254,
    255,
    256,
    257,
    258,
    469,
    470,
    471,
    472,
    473,
    474,
    509,
    512,
    513,
    514,
    515,
    516,
    859,
    860,
    861,
    862,
)


def sequence(taps):
    r = [1] * 10
    chips = []
    for _ in range(1023):
        chips.append(r[-1])
        r = [sum(r[k - 1] for k in taps) % 2] + r[:-1]
    return np.array(chips, dtype=np.int8)


def chips(prn):
    if not 1 <= prn <= 32:
        raise ValueError("PRN outside 1..32")
    g1 = sequence([3, 10])
    g2 = sequence([2, 3, 6, 8, 9, 10])
    return 1 - 2 * (g1 ^ np.roll(g2, DELAYS[prn - 1]))


def pack(z):
    def quant(v):
        return 2 * (v < 0).astype(np.uint8) + (abs(v) >= 1).astype(np.uint8)

    nibble = (quant(z.real) << 2) | quant(z.imag)
    return (nibble[0::2] | (nibble[1::2] << 4)).tobytes()


def unpack(packed):
    byte = np.frombuffer(packed, np.uint8)
    nibble = np.stack((byte & 15, byte >> 4), axis=1).ravel()
    levels = np.array([1, 3, -1, -3])
    return levels[nibble >> 2] + 1j * levels[nibble & 3]


def waveform(milliseconds, signals, noise=0.5, seed=911, alternate_bits=True):
    """signals = [(prn, initial doppler Hz, code phase chips, amplitude, Hz/s)]."""
    rng = np.random.default_rng(seed)
    code = {prn: chips(prn) for prn, *_ in signals}
    raw = bytearray()
    for ms in range(milliseconds):
        t = (ms * SAMPLES_MS + np.arange(SAMPLES_MS)) / RATE
        z = noise * (rng.standard_normal(t.size) + 1j * rng.standard_normal(t.size))
        for prn, fd, phase, amplitude, rate in signals:
            cycles = t * (fd + 0.5 * rate * t)
            phase_chips = phase + 1_023_000 * t + cycles / 1540
            idx = np.floor(phase_chips).astype(np.int64)
            data = 1 - 2 * ((idx // 20460) % 2) if alternate_bits else 1
            z += (
                amplitude
                * data
                * code[prn][idx % 1023]
                * np.exp(2j * np.pi * (IF_HZ * t + cycles + 0.125))
            )
        raw.extend(pack(z))
    return raw


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--ms", type=int, default=10)
    parser.add_argument("--prn", type=int, default=1)
    parser.add_argument("--doppler", type=float, default=-3500)
    parser.add_argument("--phase", type=float, default=123.25)
    parser.add_argument("--rate", type=float, default=0)
    args = parser.parse_args()
    with args.output.open("wb") as out:
        write_capture(
            out,
            waveform(args.ms, [(args.prn, args.doppler, args.phase, 1.0, args.rate)]),
        )
