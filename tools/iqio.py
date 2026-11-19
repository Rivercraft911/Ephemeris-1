"""Ephemeris version 1 packed complex sample recordings (SPDX-License-Identifier: Apache-2.0)."""

import struct

HEADER = struct.Struct("<4sIiIQQ")
RATE = 16_368_000
IF_HZ = 4_092_000
SAMPLES_MS = RATE // 1000
BYTES_MS = SAMPLES_MS // 2


def write_capture(stream, raw, first_sample=0):
    stream.write(HEADER.pack(b"EIQ1", RATE, IF_HZ, 1, first_sample, 2 * len(raw)))
    stream.write(raw)


def read_capture(stream):
    header = stream.read(HEADER.size)
    if len(header) != HEADER.size:
        raise ValueError("short EIQ header")
    magic, rate, carrier, encoding, first, count = HEADER.unpack(header)
    if (magic, rate, carrier, encoding) != (b"EIQ1", RATE, IF_HZ, 1):
        raise ValueError("unsupported capture format")
    raw = stream.read()
    if not count or len(raw) * 2 != count or count % SAMPLES_MS:
        raise ValueError("bad sample count or incomplete millisecond")
    return first, raw
