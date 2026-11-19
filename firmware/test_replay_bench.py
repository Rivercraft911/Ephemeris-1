"""Host-paced target protocol regressions. SPDX-License-Identifier: Apache-2.0"""
import contextlib
import csv
import io
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
import replay_bench as r


class Device:
    """Independent EPR1 responder, including a 5 Hz report before each corresponding ACK."""

    def __init__(self, bad_ack=False):
        self.lines = []
        self.bad_ack = bad_ack
        self.first = self.next = None
        self.seq = self.epochs = 0

    def reset_input_buffer(self):
        pass

    def read_until(self, suffix, maximum):
        return (self.lines.pop(0) + "\n").encode()

    def write(self, data):
        if data == b"replay 2\n":
            self.lines.extend(["READY EPR1 4 2 2", "H " + r.FIELDS])
            return len(data)
        magic, seq, first, count, flags, crc = struct.unpack("<4sIQHHI", data[:24])
        raw = data[24:]
        assert magic == b"EPB1" and flags == 0 and seq == self.seq and count <= 4
        assert len(raw) == count * r.BYTES_MS
        assert zlib.crc32(data[:20] + raw) == crc
        if not self.seq:
            self.first = self.next = first
        assert first == self.next
        if not count:
            self.lines.append(f"DONE {seq} {first}")
            return len(data)
        old = self.epochs
        self.epochs += count
        self.next += count * r.SAMPLES_MS
        self.seq += 1
        if self.epochs // 200 > old // 200:
            number = self.epochs // 200
            row = [number, self.first + number * 200 * r.SAMPLES_MS, 0, 31, 31, 31,
                   1, 500, 23422, 1, 2, 3, 4, 5, 6, 12, 0, 5, 2]
            self.lines.append("S " + ",".join(map(str, row)))
        self.lines.append(f"A {seq} {first} {count} {crc ^ self.bad_ack:08x} {2*count} 20 16")
        return len(data)


class Replay(unittest.TestCase):
    def test_integrity_pacing_stop_and_drain(self):
        with tempfile.TemporaryFile() as source:
            n = 622 * r.SAMPLES_MS
            source.write(r.HEADER.pack(b"EIQ1", r.RATE, r.IF_HZ, 1, 17, n))
            source.write(bytes(n // 2))
            source.seek(0)
            output = io.StringIO()
            with contextlib.redirect_stderr(io.StringIO()):
                m = r.replay(Device(), source, output, 2, stop_fixes=2)
            self.assertEqual(m["final_consecutive_valid"], 2)
            self.assertEqual(m["longest_valid_streak_reports"], 2)
            self.assertGreater(m["longest_valid_streak_wall_hz"], 0)
            self.assertAlmostEqual(m["recording_seconds_per_wall_second"] * r.RATE / 2,
                                   m["raw_input_bytes_per_wall_second"], places=2)
            self.assertIn(m["processed_ms"], range(400, 409))
            self.assertEqual(len(list(csv.DictReader(io.StringIO(output.getvalue())))), 2)
            source.seek(0)
            with self.assertRaisesRegex(ValueError, "acknowledgement"):
                r.replay(Device(True), source, io.StringIO(), 2, max_ms=1)

    def test_wall_rate_uses_longest_unbroken_valid_streak(self):
        a = r.FixArrivals()
        self.assertIsNone(a.summary()["longest_valid_streak_wall_hz"])
        for t, valid in [(1, True), (4, True), (6, False), (10, True), (14, True),
                         (19, True), (20, False), (25, True)]:
            a.observe(valid, t)
        m = a.summary()
        self.assertEqual((m["first_valid_host_offset_s"], m["last_valid_host_offset_s"]), (1, 25))
        self.assertEqual(m["longest_valid_streak_reports"], 3)
        self.assertEqual(m["longest_valid_streak_wall_hz"], 2 / 9)
        self.assertEqual(m["longest_valid_streak_wall_s"], 9)

    def test_short_or_overstated_recording(self):
        with tempfile.TemporaryFile() as f:
            f.write(b"EIQ1")
            f.seek(0)
            with self.assertRaisesRegex(ValueError, "short EIQ"):
                r.capture_header(f)
            f.seek(0)
            f.write(r.HEADER.pack(b"EIQ1", r.RATE, r.IF_HZ, 1, 0, r.SAMPLES_MS))
            f.seek(0)
            with self.assertRaisesRegex(ValueError, "file length"):
                r.capture_header(f)


if __name__ == "__main__":
    unittest.main()
