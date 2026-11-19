#!/usr/bin/env python3
"""Run a cold, host-paced EIQ1 recording on the Pico replay image. SPDX-License-Identifier: Apache-2.0"""
import argparse
import csv
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from iqio import BYTES_MS, HEADER, IF_HZ, RATE, SAMPLES_MS

META = struct.Struct("<4sIQHH")
CRC = struct.Struct("<I")
FIELDS = ("sequence,sample,gaps,active_mask,locked_mask,nav_mask,valid,week_mod1024,tow_s,"
          "x_m,y_m,z_m,vx_mps,vy_mps,vz_mps,bias_m,drift_mps,sv,pdop")


def frame(seq, first, count, payload):
    head = META.pack(b"EPB1", seq, first, count, 0)
    crc = zlib.crc32(payload, zlib.crc32(head))
    return head + CRC.pack(crc) + payload, crc


def capture_header(source):
    header = source.read(HEADER.size)
    if len(header) != HEADER.size:
        raise ValueError("short EIQ header")
    magic, rate, carrier, encoding, first, count = HEADER.unpack(header)
    if (magic, rate, carrier, encoding) != (b"EIQ1", RATE, IF_HZ, 1):
        raise ValueError("unsupported capture format")
    if not count or count % SAMPLES_MS or count + first >= 1 << 64:
        raise ValueError("invalid capture interval")
    if os.fstat(source.fileno()).st_size != HEADER.size + count // 2:
        raise ValueError("file length differs from EIQ1 sample count")
    return first, count // SAMPLES_MS


def read_line(port):
    raw = port.read_until(b"\n", 512)
    if not raw.endswith(b"\n"):
        raise ValueError(f"missing or incomplete target response: {raw[:80]!r}")
    line = raw.decode("ascii").rstrip("\r\n")
    if line.startswith("ERR"):
        raise ValueError(f"target rejected replay: {line}")
    return line


def send(port, data):
    if port.write(data) != len(data):
        raise OSError("short USB write")


class FixArrivals:
    """Host-monotonic arrival times for contiguous 5 Hz reports marked valid by the target."""

    def __init__(self):
        self.first = self.last = self.run_start = None
        self.best_start = self.best_end = None
        self.run = self.best = 0

    def observe(self, valid, elapsed):
        if not valid:
            self.run = 0
            return
        if self.first is None:
            self.first = elapsed
        self.last = elapsed
        if not self.run:
            self.run_start = elapsed
        self.run += 1
        if self.run > self.best:
            self.best = self.run
            self.best_start = self.run_start
            self.best_end = elapsed

    def summary(self):
        span = None if not self.best else self.best_end - self.best_start
        return dict(first_valid_host_offset_s=self.first, last_valid_host_offset_s=self.last,
                    longest_valid_streak_reports=self.best, longest_valid_streak_wall_s=span,
                    longest_valid_streak_wall_hz=(self.best-1)/span if self.best>1 and span>0 else None)


def replay(port, source, output, work, stop_fixes=0, max_ms=None):
    first, total = capture_header(source)
    limit = min(total, max_ms) if max_ms is not None else total
    port.reset_input_buffer()
    send(port, f"replay {work}\n".encode("ascii"))
    if read_line(port) != f"READY EPR1 4 2 {work}" or read_line(port) != "H " + FIELDS:
        raise ValueError("unexpected replay image or schema")

    output.write(FIELDS + "\n")
    output.flush()
    outstanding = deque()
    sent_ms = done_ms = seq = rows = consecutive = steps = tracking_us = acquisition_us = 0
    first_valid_ms = None
    digest = hashlib.sha256()
    stopped = False
    arrivals = FixArrivals()
    started = next_progress = time.monotonic()
    while outstanding or (sent_ms < limit and not stopped):
        while len(outstanding) < 2 and sent_ms < limit and not stopped:
            count = min(4, limit - sent_ms)
            raw = source.read(count * BYTES_MS)
            if len(raw) != count * BYTES_MS:
                raise ValueError("short capture payload")
            sample = first + sent_ms * SAMPLES_MS
            packet, crc = frame(seq, sample, count, raw)
            send(port, packet)
            outstanding.append((seq, sample, count, crc))
            digest.update(raw)
            seq += 1
            sent_ms += count
        line = read_line(port)
        if line.startswith("S "):
            received_at = time.monotonic() - started
            text = line[2:]
            row = dict(zip(FIELDS.split(","), next(csv.reader([text])), strict=True))
            rows += 1
            expected_sample = first + rows * 200 * SAMPLES_MS
            if (int(row["sequence"]) != rows or int(row["sample"]) != expected_sample
                    or int(row["gaps"]) != 0 or row["valid"] not in ("0", "1")):
                raise ValueError(f"non-contiguous 5 Hz target status: {text}")
            arrivals.observe(row["valid"] == "1", received_at)
            if row["valid"] == "1":
                if first_valid_ms is None:
                    first_valid_ms = rows * 200
                consecutive += 1
            else:
                consecutive = 0
            if stop_fixes and consecutive >= stop_fixes:
                stopped = True
            output.write(text + "\n")
            output.flush()
            continue
        ack = line.split()
        if len(ack) != 8 or ack[0] != "A":
            raise ValueError(f"unexpected frame response: {line}")
        got = int(ack[1]), int(ack[2]), int(ack[3]), int(ack[4], 16)
        if not outstanding or got != outstanding.popleft():
            raise ValueError("target acknowledgement differs from transmitted frame")
        done_ms += got[2]
        work_done, track, search = map(int, ack[5:])
        if not 0 <= work_done <= work * got[2] or min(track, search) < 0:
            raise ValueError("invalid target timing or work count")
        steps += work_done
        tracking_us += track
        acquisition_us += search
        now = time.monotonic()
        if now >= next_progress:
            print(f"{done_ms / 1000:.3f} s of IQ, {now - started:.1f} s wall, "
                  f"{steps} search steps, {rows} status rows, {consecutive} successive fixes",
                  file=sys.stderr, flush=True)
            next_progress = now + 15
    sample = first + done_ms * SAMPLES_MS
    end, _ = frame(seq, sample, 0, b"")
    send(port, end)
    if read_line(port) != f"DONE {seq} {sample}" or rows != done_ms // 200:
        raise ValueError("incomplete target drain or missing 5 Hz status")
    elapsed = time.monotonic() - started
    return dict(mode="host-paced-cold-replay", first_sample=first, sample_rate_hz=RATE,
                capture_ms=total, processed_ms=done_ms, frames=seq, committed_raw_sha256=digest.hexdigest(),
                work_per_ms=work, search_steps=steps, tracking_us=tracking_us,
                acquisition_us=acquisition_us, status_rows=rows, first_valid_ms=first_valid_ms,
                final_consecutive_valid=consecutive, elapsed_s=elapsed,
                recording_seconds_per_wall_second=done_ms/(1000*elapsed),
                raw_input_bytes_per_wall_second=done_ms*BYTES_MS/elapsed,
                scheduled_status_hz_in_recording_time=5, **arrivals.summary())


def score_replay(truth_path, receiver_path, result, sample_zero_s):
    """Apply independent truth after the USB port closes, over the actual replay interval."""
    from campaign import score
    with receiver_path.open() as stream:
        receiver = list(csv.DictReader(stream))
    if not receiver:
        raise ValueError("no 5 Hz target output to score")
    stop = sample_zero_s + int(receiver[-1]["sample"]) / RATE
    with truth_path.open() as stream:
        truth = [r for r in csv.DictReader(stream) if float(r["t_s"]) <= stop + 1e-7]
    if not truth or abs(float(truth[-1]["t_s"]) - stop) > 1e-6:
        raise ValueError("independent truth must cover the last reported 5 Hz epoch")
    gps = None
    if "gps_week" in truth[0] and "tow_s" in truth[0]:
        gps = int(truth[0]["gps_week"]), float(truth[0]["tow_s"])
    measured = score(truth, receiver, sample_zero_s, endurance=False, gps_start=gps)
    measured.update(scope="host-paced target replay", scored_end_s=stop,
                    processed_ms=result["processed_ms"])
    return measured


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("port")
    cli.add_argument("capture", type=Path)
    cli.add_argument("output", type=Path, help="target-only 5 Hz CSV")
    cli.add_argument("--work", type=int, default=2, choices=range(1, 129), metavar="1..128")
    cli.add_argument("--stop-after-fixes", type=int, default=0, help="stop after N consecutive receiver-valid reports")
    cli.add_argument("--max-ms", type=int, help="replay at most this many whole milliseconds")
    cli.add_argument("--metrics", type=Path, help="write separate transport/timing JSON")
    cli.add_argument("--truth", type=Path, help="score the recorded interval against independent truth after USB closes")
    cli.add_argument("--sample-zero-s", type=float, help="time in truth seconds at raw ADC index zero")
    args = cli.parse_args()
    if (args.truth is None) != (args.sample_zero_s is None):
        cli.error("--truth and --sample-zero-s must be supplied together")
    if args.stop_after_fixes < 0 or args.max_ms is not None and args.max_ms <= 0:
        cli.error("limits must be positive")
    import serial
    partial = args.output.with_name(args.output.name + ".partial")
    paths = [args.capture, args.output, partial, args.metrics, args.truth]
    resolved = [p.resolve() for p in paths if p is not None]
    if len(set(resolved)) != len(resolved):
        cli.error("capture, output, partial, metrics, and truth files must be distinct")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with (args.capture.open("rb") as source, partial.open("w", newline="") as out,
              serial.Serial(args.port, 115200, timeout=45, write_timeout=45) as port):
            result = replay(port, source, out, args.work, args.stop_after_fixes, args.max_ms)
        os.replace(partial, args.output)
    except (ValueError, OSError, serial.SerialException) as error:
        raise SystemExit(f"{error}. Incomplete evidence is in {partial}") from error
    try:
        if args.truth:
            result["truth_score"] = score_replay(args.truth, args.output, result, args.sample_zero_s)
        if args.metrics:
            args.metrics.parent.mkdir(parents=True, exist_ok=True)
            args.metrics.write_text(json.dumps(result, indent=2) + "\n")
    except (ValueError, OSError) as error:
        raise SystemExit(f"{error}. Target statuses are in {args.output}") from error
    print(json.dumps(result), file=sys.stderr)
    if args.stop_after_fixes and result["final_consecutive_valid"] < args.stop_after_fixes:
        raise SystemExit("recording ended before requested consecutive fixes")
    if args.truth and not result["truth_score"]["passed"]:
        raise SystemExit("recorded interval failed the diagnostic truth score")


if __name__ == "__main__":
    main()
