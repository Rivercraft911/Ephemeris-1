#!/usr/bin/env python3
"""Convert recordings to and from the lossy half-chip transport for the clocked Pico bench.

Each 512-byte millisecond holds 2046 little-endian sign pairs, followed by
two zero pairs. Bit 0 is negative I and bit 1 negative Q. The compact file is
a headerless bench payload. Expansion reconstructs a unit-amplitude EIQ1 input
for comparison with native replay. SPDX-License-Identifier: Apache-2.0
"""

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import threading
import time
import zlib

import numpy as np

from replay_bench import BYTES_MS, FIELDS, HEADER, IF_HZ, RATE, SAMPLES_MS, FixArrivals, capture_header, read_line, send

HALF_CHIPS = SAMPLES_MS // 8
COMPACT_BYTES = (HALF_CHIPS + 3) // 4
BATCH_MS = 128
MAX_CLOCKED_MS = 600000


def pack_block(raw):
    """Downmix and sum eight ADC samples per sign pair, with zero mapped positive."""
    samples = np.frombuffer(raw, np.uint8).reshape(-1, HALF_CHIPS, 4)
    level = np.array([1, 3, -1, -3], dtype=np.int8)
    byte = np.arange(256)
    # Each byte contributes x[0] - j*x[1]. The next byte has the opposite sign.
    pair_i = level[(byte >> 2) & 3] + level[(byte >> 4) & 3]
    pair_q = level[byte & 3] - level[(byte >> 6) & 3]
    def integrate(table):
        a, b, c, d = (table[samples[:, :, i]] for i in range(4))
        return a - b + c - d

    signs = np.zeros((len(samples), COMPACT_BYTES * 4), dtype=np.uint8)
    signs[:, :HALF_CHIPS] = (integrate(pair_i) < 0) | ((integrate(pair_q) < 0).astype(np.uint8) << 1)
    pairs = signs.reshape(-1, COMPACT_BYTES, 4)
    return (pairs[:, :, 0] | (pairs[:, :, 1] << 2) |
            (pairs[:, :, 2] << 4) | (pairs[:, :, 3] << 6)).tobytes()


def expand_block(packed):
    """Hold each ±1±j baseband value for eight samples and remix at +Fs/4."""
    block = np.frombuffer(packed, np.uint8).reshape(-1, COMPACT_BYTES)
    if np.any(block[:, -1] & 0xf0):
        raise ValueError("nonzero half-chip padding")
    shifts = np.array([0, 2, 4, 6], dtype=np.uint8)
    signs = ((block[:, :, None] >> shifts) & 3).reshape(len(block), -1)[:, :HALF_CHIPS]
    words = np.zeros(4, dtype="<u4")
    for pair in range(4):
        i, q = pair & 1, pair >> 1
        # Each axis occupies two ADC bits: 00 is +1, 10 is -1.
        turn = 0
        for phase, (si, sq) in enumerate(((i, q), (1-q, i), (1-i, 1-q), (q, 1-i))):
            turn |= ((si << 3) | (sq << 1)) << (4 * phase)
        words[pair] = turn | (turn << 16)
    return words[signs].tobytes()


def convert(mode, source, destination):
    with source.open("rb") as src:
        if mode == "pack":
            origin, ms = capture_header(src)
            if origin:
                raise ValueError("clocked replay requires an EIQ1 capture starting at sample zero")
            stride, encode = BYTES_MS, pack_block
        elif mode == "expand":
            ms, remainder = divmod(os.fstat(src.fileno()).st_size, COMPACT_BYTES)
            if not ms or remainder or ms * SAMPLES_MS >= 1 << 64:
                raise ValueError("invalid half-chip payload length")
            stride, encode = COMPACT_BYTES, expand_block
        else:
            raise ValueError("unknown conversion mode")

        with destination.open("xb") as out:
            try:
                if mode == "expand":
                    out.write(HEADER.pack(b"EIQ1", RATE, IF_HZ, 1, 0, ms * SAMPLES_MS))
                for start in range(0, ms, BATCH_MS):
                    size = min(BATCH_MS, ms - start) * stride
                    data = src.read(size)
                    if len(data) != size:
                        raise ValueError("short input block")
                    out.write(encode(data))
                if src.read(1):
                    raise ValueError("trailing input")
                out.flush()
            except BaseException:
                out.close()
                destination.unlink()
                raise
    return ms


def inspect_payload(source):
    """Hash and validate the complete local payload before starting the device clock."""
    size = os.fstat(source.fileno()).st_size
    epochs, tail = divmod(size, COMPACT_BYTES)
    if tail or not 0 < epochs <= MAX_CLOCKED_MS:
        raise ValueError("clocked replay requires 1 to 600000 complete millisecond frames")
    digest, crc = hashlib.sha256(), 0
    for chunk in iter(lambda: source.read(BATCH_MS * COMPACT_BYTES), b""):
        if len(chunk) % COMPACT_BYTES or any(byte & 0xf0 for byte in chunk[COMPACT_BYTES-1::COMPACT_BYTES]):
            raise ValueError("bad half-chip frame or padding")
        digest.update(chunk)
        crc = zlib.crc32(chunk, crc)
    source.seek(0)
    return epochs, crc, digest.hexdigest()


def run(port, source, out):
    epochs, crc, sha = inspect_payload(source)
    port.reset_input_buffer()
    send(port, f"clocked {epochs} {crc:08x}\n".encode("ascii"))
    if read_line(port) != "READY CLK1":
        raise ValueError("unexpected clocked-replay handshake")
    started = time.monotonic()
    stop = threading.Event()
    errors = []

    def write_samples():
        try:
            for chunk in iter(lambda: source.read(COMPACT_BYTES * 8), b""):
                if stop.is_set():
                    return
                send(port, chunk)
        except Exception as exc:
            errors.append(exc)

    writer = threading.Thread(target=write_samples, daemon=True)
    writer.start()
    rows = valid_rows = gaps = 0
    previous_us = 0
    max_status_delay_us = 0
    min_interval = max_interval = None
    arrivals = FixArrivals()
    events = {k: None for k in ("first_lock", "four_locks", "first_valid")}
    try:
        if read_line(port) != "H " + FIELDS:
            raise ValueError("unexpected 5 Hz status header")
        out.write(FIELDS + "\n")
        while True:
            if errors:
                raise errors[0]
            line = read_line(port)
            host_offset = time.monotonic() - started
            if line.startswith("PASS "):
                break
            if not line.startswith("S "):
                raise ValueError(f"clocked replay failed: {line}")
            _, device_time, status = line.split(" ", 2)
            device_us = int(device_time)
            row = dict(zip(FIELDS.split(","), next(csv.reader([status])), strict=True))
            rows += 1
            sample = int(row["sample"])
            delay_us = device_us - rows * 200000
            if (int(row["sequence"]) != rows or sample != rows * (RATE // 5)
                    or device_us <= previous_us or delay_us < 0 or row["valid"] not in ("0", "1")):
                raise ValueError(f"out-of-order or invalid target status: {line}")
            max_status_delay_us = max(max_status_delay_us, delay_us)
            if rows > 1:
                elapsed = device_us - previous_us
                min_interval = elapsed if min_interval is None else min(min_interval, elapsed)
                max_interval = elapsed if max_interval is None else max(max_interval, elapsed)
            previous_us = device_us
            previous_gaps, gaps = gaps, int(row["gaps"])
            locked = int(row["locked_mask"])
            active, navigation = int(row["active_mask"]), int(row["nav_mask"])
            if not (0 <= locked <= 0xffffffff and 0 <= active <= 0xffffffff
                    and 0 <= navigation <= 0xffffffff and locked & ~active == 0
                    and navigation & ~locked == 0 and previous_gaps <= gaps):
                raise ValueError("invalid channel masks or gap counter")
            valid = row["valid"] == "1"
            arrivals.observe(valid, host_offset)
            if valid:
                coordinates = [float(row[k]) for k in ("x_m", "y_m", "z_m", "vx_mps", "vy_mps", "vz_mps",
                                                        "bias_m", "drift_mps", "pdop", "tow_s")]
                if not 4 <= int(row["sv"]) <= navigation.bit_count() or not all(map(math.isfinite, coordinates)):
                    raise ValueError("invalid navigation solution marked valid")
                valid_rows += 1
            for key, reached in (("first_lock", bool(locked)), ("four_locks", locked.bit_count() >= 4),
                                 ("first_valid", valid)):
                if reached and events[key] is None:
                    events[key] = dict(sample_s=sample/RATE, device_s=device_us/1e6, host_s=host_offset)
            out.write(status + "\n")
            out.flush()
        summary = dict(item.split("=", 1) for item in line.split()[1:])
        if (int(summary["epochs"]) != epochs or int(summary["crc"], 16) != crc
                or int(summary["starved"]) != 0 or gaps or rows != epochs // 200
                or int(summary["elapsed_us"]) < max(previous_us, epochs * 1000)
                or not max_status_delay_us <= int(summary["late_us"]) < 4000):
            raise ValueError(f"invalid clocked transport summary: {line}")
        writer.join(timeout=3)
        if writer.is_alive() or errors or source.tell() != epochs * COMPACT_BYTES:
            raise ValueError(f"target finished before host transmission completed: {errors}")
    finally:
        stop.set()
        if writer.is_alive():
            if hasattr(port, "cancel_write"):
                port.cancel_write()
            writer.join(timeout=3)
    return {
        "mode": "clocked half-chip USB replay", "epochs": epochs,
        "compact_sha256": sha, "compact_crc32": f"{crc:08x}",
        "origin": "device elapsed starts after prefill; host elapsed starts at READY",
        "events": events, "status_rows": rows, "valid_rows": valid_rows, "gaps": gaps,
        "device_elapsed_s": int(summary["elapsed_us"])/1e6, "host_elapsed_s": host_offset,
        "min_device_status_interval_us": min_interval, "max_device_status_interval_us": max_interval,
        "late_us": int(summary["late_us"]), "starved": int(summary["starved"]),
        "peak_backlog": int(summary["peak_backlog"]), **arrivals.summary(),
    }


def run_files(args):
    import serial

    metrics = args.metrics or args.output.with_suffix(".json")
    partial = args.output.with_name(args.output.name + ".partial")
    paths = [args.input, args.output, metrics, partial]
    if len({p.resolve() for p in paths}) != len(paths) or any(p.exists() for p in paths[1:]):
        raise ValueError("use distinct new output, partial and metrics paths")
    with (args.input.open("rb") as source, serial.Serial(args.port, 115200, timeout=8, write_timeout=8) as port,
          partial.open("x", newline="") as out):
        result = run(port, source, out)
    metrics.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    partial.rename(args.output)
    print(json.dumps(result, allow_nan=False))


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    subs = cli.add_subparsers(dest="mode", required=True)
    for mode in ("pack", "expand"):
        sub = subs.add_parser(mode)
        sub.add_argument("input", type=Path)
        sub.add_argument("output", type=Path)
    device = subs.add_parser("run", help="stream a complete compact file through the clocked Pico bench")
    device.add_argument("port")
    device.add_argument("input", type=Path)
    device.add_argument("output", type=Path, help="new 5 Hz receiver CSV")
    device.add_argument("--metrics", type=Path, help="timing JSON (default: output with .json suffix)")
    args = cli.parse_args()
    try:
        if args.mode == "run":
            run_files(args)
        else:
            print(f"{convert(args.mode, args.input, args.output)} ms")
    except (OSError, ValueError, KeyError, ImportError) as exc:
        cli.exit(1, f"{exc}\n")
