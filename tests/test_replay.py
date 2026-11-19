"""Independent packed-file and live/replay parity checks (SPDX-License-Identifier: Apache-2.0)."""

import csv
import hashlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from iqio import HEADER, SAMPLES_MS, read_capture, write_capture
from vectors import chips, pack, unpack, waveform
from tone import inspect

REPLAY = sys.argv.pop(1)


class Replay(unittest.TestCase):
    def test_known_bytes(self):
        data = (ROOT / "tests/fixtures/prn01.eiq").read_bytes()
        self.assertEqual(
            hashlib.sha256(data).hexdigest(),
            "4a3befd50829c53215bfd5c9940362008ff295f8eccfe8a2d4465acb43f1e515",
        )
        start, raw = read_capture(io.BytesIO(data))
        self.assertEqual(start, 0)
        self.assertEqual(len(raw), 8184)
        self.assertEqual(raw, waveform(1, [(1, -3500.0, 123.25, 1.0, 0.0)], noise=0))
        self.assertEqual(
            int("".join(str(int(c < 0)) for c in chips(1)[:10]), 2), 0o1440
        )
        # Expected nibble e.g. 1001 => I=-1, Q=+3. First sample is the LOW nibble.
        self.assertEqual(list(unpack(bytes([0x79]))), [-1 + 3j, 3 - 3j])
        self.assertEqual(pack(unpack(bytes(range(256))) / 2), bytes(range(256)))
        for broken in (data[:25], data[:-1], data + b"a", b"oops" + data[4:]):
            self.assertNotEqual(
                subprocess.run(
                    [REPLAY, "-"], input=broken, capture_output=True
                ).returncode,
                0,
            )

    def test_conducted_tone_faults(self):
        z = np.exp(2j * np.pi * (np.arange(SAMPLES_MS) * 0.25 + 0.125))
        self.assertTrue(inspect(pack(z))["passed"])
        self.assertFalse(
            inspect(pack(z.imag + 1j * z.real))["passed"]
        )  # swapped I and Q
        self.assertFalse(
            inspect(pack(z.real - 1j * z.imag))["passed"]
        )  # inverted Q sign
        self.assertFalse(inspect(waveform(1, [], seed=917))["passed"])

    def check_acquisition(self, ms, doppler):
        # The receiver gets no PRN, phase or Doppler input. Truth stays in Python.
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.eiq"
            log = Path(d) / "channels.csv"
            with path.open("wb") as out:
                write_capture(
                    out, waveform(ms, [(1, doppler, 77.125, 1.3, 55.0)], seed=4567)
                )
            result = subprocess.run(
                [REPLAY, str(path), "--work", "128", "--channels", str(log)],
                check=True,
                capture_output=True,
                text=True,
            )
            with log.open() as f:
                rows = list(csv.DictReader(f))
            locked = [r for r in rows if r["prn"] == "1" and r["locked"] == "1"]
            self.assertGreater(len(locked), 35)
            expected_doppler = doppler + 55 * ms / 1000
            self.assertLess(abs(float(locked[-1]["doppler_hz"]) - expected_doppler), 175)
            status = csv.DictReader(io.StringIO(result.stdout))
            self.assertFalse(any(int(row["valid"]) for row in status))

    def test_unaided_acquisition(self):
        self.check_acquisition(90, -6320.0)

    def test_full_doppler_coverage(self):
        # The ground-first build scans the other PRNs before its full-band pass.
        self.check_acquisition(500, -18320.0)

    def test_fractional_search_budget(self):
        block = (ROOT / "tests/fixtures/prn01.eiq").read_bytes()[HEADER.size :]

        def replay(ms, rate):
            stream = io.BytesIO()
            write_capture(stream, block * ms)
            return subprocess.run(
                [REPLAY, "-", "--work", rate], input=stream.getvalue(), capture_output=True
            )

        # The first input is ready immediately. Credit permits one step every four epochs.
        for ms, expect in ((3, 0), (4, 1), (7, 1), (8, 2)):
            result = replay(ms, "1/4")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"search_steps={expect} ".encode(), result.stderr)
        for bad in ("-1", "1/0", "1/2/3", "4097", "1.5"):
            self.assertNotEqual(replay(1, bad).returncode, 0)

    def test_noise_only(self):
        with tempfile.TemporaryDirectory() as d:
            record = Path(d) / "n.eiq"
            with record.open("wb") as f:
                write_capture(f, waveform(201, [], seed=709))
            result = subprocess.run(
                [REPLAY, str(record), "--work", "48"],
                capture_output=True,
                text=True,
                check=True,
            )
            rows = list(csv.DictReader(io.StringIO(result.stdout)))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["valid"], "0")
            self.assertEqual(rows[0]["locked_mask"], "0")
            self.assertEqual(int(rows[0]["sample"]), 200 * SAMPLES_MS)

    def test_strong_signal_cross_code_sidelobes(self):
        # Repeated capture reproduces the on-chip blind-lock bench. Varying
        # noise and a second, 18 dB weaker signal exercise the same issue with
        # distinct input epochs. The receiver sees only packed four-bit I/Q.
        strong = [(4, 0.0, 137.25, 1.0, 0.0)]
        pair = [(4, -2350.0, 142.25, 1.0, 0.0), (7, 4900.0, 23.45, 0.125, 0.0)]
        for name, signals, raw in (
            ("repeated", strong, waveform(1, strong, noise=.05, alternate_bits=False) * 350),
            ("near-far", pair, waveform(500, pair, noise=.38, seed=421)),
        ):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                record, log = Path(directory) / "input.eiq", Path(directory) / "channels.csv"
                with record.open("wb") as stream:
                    write_capture(stream, raw)
                subprocess.run([REPLAY, str(record), "--work", "64", "--channels", str(log)],
                               capture_output=True, check=True)
                with log.open() as stream:
                    locks = [r for r in csv.DictReader(stream) if r["locked"] == "1"]
                self.assertEqual({int(r["prn"]) for r in locks}, {s[0] for s in signals})
                for prn, fd, phase, _, _ in signals:
                    matching = [r for r in locks if int(r["prn"]) == prn]
                    self.assertGreaterEqual(len(matching), 20)
                    row = matching[-1]
                    # The reported code phase is the end of this millisecond.
                    time_s = (int(row["sample"]) / SAMPLES_MS + 1) * .001
                    expected = phase + fd * time_s / 1540
                    phase_error = (float(row["code_phase_chips"]) - expected + 511.5) % 1023 - 511.5
                    self.assertLess(abs(phase_error), .75)
                    self.assertLess(abs(float(row["doppler_hz"]) - fd), 100)


if __name__ == "__main__":
    unittest.main()
