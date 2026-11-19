#!/usr/bin/env python3
"""Collect an Ephemeris bring-up capture or compare the four bitplane lanes. SPDX-License-Identifier: Apache-2.0"""
import argparse
import csv
import math
import io
import os
from pathlib import Path
import sys
import subprocess
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from iqio import BYTES_MS, HEADER, IF_HZ, RATE, SAMPLES_MS, read_capture


def pattern_cycle():
    """Independent Fibonacci-form bit recurrence of the fixture's Galois PRBS16."""
    bits = bytearray((0x6801 >> i) & 1 for i in range(16))
    for i in range(4 * 65536 - 16):
        bits.append(bits[i] ^ bits[i + 2] ^ bits[i + 3] ^ bits[i + 5])
    return bytes(bits[i] | bits[i + 1] << 1 | bits[i + 2] << 2 | bits[i + 3] << 3
                 for i in range(0, len(bits), 4))


def verify_pattern(raw):
    if len(raw) < 16:
        raise ValueError("pattern fragment too short")
    samples = bytearray(2 * len(raw))
    samples[0::2] = bytes(b & 15 for b in raw)
    samples[1::2] = bytes(b >> 4 for b in raw)
    cycle = pattern_cycle()
    # Nibble order is fixed. Only the independently clocked starting phase is unknown.
    reference = cycle * (2 + len(samples) // len(cycle))
    start = reference.find(samples[:32], 0, len(cycle) + 31)
    if start < 0:
        raise ValueError("no PRBS phase matches the first 32 samples")
    expected = reference[start:start + len(samples)]
    if samples != expected:
        i = next(i for i, (a, b) in enumerate(zip(samples, expected)) if a != b)
        raise ValueError(f"PRBS mismatch at sample {i}: got {samples[i]}, expected {expected[i]}")
    return start


def run(port, action, output, prbs, internal=False):
    port.reset_input_buffer()
    port.write((("self" if internal else "") + action + "\n").encode())
    if action == "compare":
        result = port.readline().decode("ascii", errors="replace").strip()
        if not result.startswith("PASS samples=163680 mismatches=0 "):
            raise ValueError(result or "no bitplane comparison response")
        print(result)
        return
    first = port.read(4)
    if first == b"ERR ":
        raise ValueError(port.readline().decode("ascii", errors="replace").strip())
    if first != b"EIQ1":
        raise ValueError(f"expected EIQ1 response, received {first!r}")
    frame = first + port.read(HEADER.size - 4 + 10 * BYTES_MS)
    _, raw = read_capture(io.BytesIO(frame))
    if len(raw) != 10 * BYTES_MS:
        raise ValueError("expected a ten-millisecond capture")
    if prbs:
        print(f"PRBS matched {len(raw) * 2} samples at phase {verify_pattern(raw)}")
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Publish only a complete, verified capture. An interrupted transfer leaves the old file intact.
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as tmp:
        name = tmp.name
    try:
        Path(name).write_bytes(frame)
        os.replace(name, output)
    finally:
        Path(name).unlink(missing_ok=True)
    print(f"{output}: {len(raw)} packed sample bytes")



def compare_native(executable, channels, raw, summary, observed):
    def fields(line):
        return dict(part.split("=", 1) for part in line.split()[1:])
    meta = fields(summary)
    # The first device epoch clears tracking. Exactly 999 subsequent epochs use
    # the injected zero-Doppler seeds. Preserve the same absolute sample clock.
    count = int(meta["epochs"]) - 1
    first = int(meta["first"]) + SAMPLES_MS
    header = HEADER.pack(b"EIQ1", RATE, IF_HZ, 1, first, count * SAMPLES_MS)
    with tempfile.TemporaryDirectory() as folder:
        rows_path = Path(folder) / "channels.csv"
        command = [str(executable), "-", "--work", "0", "--channels", str(rows_path)]
        for i in range(channels):
            command += ["--seed", f"{i + 1},0,{i * 37}"]
        subprocess.run(command, input=header + raw * count, check=True, stdout=subprocess.DEVNULL)
        with rows_path.open() as stream:
            reference = {int(row["prn"]): row for row in csv.DictReader(stream)}
    if len(observed) != channels or len(reference) != channels:
        raise ValueError("tracking channel count differs from the desktop replay")
    for line in observed:
        actual = fields(line)
        golden = reference[int(actual["prn"])]
        dc = math.remainder(float(actual["phase_chips"]) - float(golden["code_phase_chips"]), 1023)
        df = float(actual["doppler_hz"]) - float(golden["doppler_hz"])
        prompt = complex(float(actual["prompt_i"]), float(actual["prompt_q"]))
        expected = complex(float(golden["i"]), float(golden["q"]))
        if (int(actual["sample"]) != int(golden["sample"]) or golden["locked"] != "1"
                or not abs(dc) <= 0.05 or not abs(df) <= 1.0
                or not abs(prompt - expected) <= 0.05 * abs(expected)):
            raise ValueError(f"native/device disagreement on PRN {actual['prn']}")
    print(f"All {channels} final tracking epochs matched the native receiver tolerances.")


def profile(port, channels, external, reference=None):
    # All code epochs repeat in exactly 1 ms at zero Doppler. Seed phase agrees
    # with the waveform generator at its first raw sample. Live builds have no seed command.
    from vectors import waveform
    signals = [(i + 1, 0.0, i * 37.0, 1.0, 0.0) for i in range(channels)]
    raw = waveform(1, signals, noise=0.05, alternate_bits=False)
    if len(raw) != BYTES_MS:
        raise ValueError("bad replay vector length")
    port.reset_input_buffer()
    mode = "external" if external else "profile"
    port.write(f"{mode} {channels}\n".encode())
    ready = port.readline().strip()
    if ready != b"READY 8184":
        raise ValueError(f"unexpected profile handshake {ready!r}")
    port.write(raw)
    result = port.readline().decode("ascii", errors="replace").strip()
    print(result)
    observed = []
    for _ in range(9):
        line = port.readline().decode("ascii", errors="replace").strip()
        if not line:
            raise ValueError("profile response timed out")
        if line == "END":
            break
        print(line)
        observed.append(line)
    else:
        raise ValueError("missing profile terminator")
    if not result.startswith("PASS "):
        raise ValueError("receiver failed the printed profile criteria")
    if reference is not None:
        compare_native(reference, channels, raw, result, observed)

if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("port", help="serial device for ephemeris_capture.uf2")
    sub = cli.add_subparsers(dest="action", required=True)
    snap = sub.add_parser("capture")
    snap.add_argument("output", type=Path)
    snap.add_argument("--prbs", action="store_true", help="check independent second-Pico fixture")
    snap.add_argument("--internal", action="store_true", help="capture an internal PIO PRBS source without wiring")
    cmp = sub.add_parser("compare")
    cmp.add_argument("--internal", action="store_true", help="compare all five PIO receive lanes on the internal source")
    prof = sub.add_parser("profile")
    prof.add_argument("channels", type=int, choices=(1, 4, 8))
    prof.add_argument("--external", action="store_true", help="use an independently clocked fixture on GP6")
    prof.add_argument("--reference", type=Path, help="native eph-replay executable for numerical comparison")
    args = cli.parse_args()
    import serial  # pyserial is only required for the physical bench
    try:
        with serial.Serial(args.port, 115200, timeout=9, write_timeout=2) as device:
            if args.action == "profile":
                profile(device, args.channels, args.external, args.reference)
            else:
                run(device, args.action, getattr(args, "output", None), getattr(args, "prbs", False), args.internal)
    except (OSError, ValueError, serial.SerialException) as exc:
        sys.exit(str(exc))
