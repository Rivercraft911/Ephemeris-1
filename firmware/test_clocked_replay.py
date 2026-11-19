"""Verify the half-chip USB fixture against direct complex mixing. SPDX-License-Identifier: Apache-2.0"""

import io
from pathlib import Path
import queue
import zlib
import random
import tempfile
import unittest

from clocked_replay import BYTES_MS, COMPACT_BYTES, HALF_CHIPS, convert, expand_block, pack_block, run
from replay_bench import FIELDS, HEADER, IF_HZ, RATE, SAMPLES_MS


class ClockedReplay(unittest.TestCase):
    def test_downmix_and_tie(self):
        rng = random.Random(2806)
        data = rng.randbytes(BYTES_MS * 2)
        packed = pack_block(data)
        self.assertEqual(len(packed), 2 * COMPACT_BYTES)
        levels = (1, 3, -1, -3)
        for ms in range(2):
            for h in range(HALF_CHIPS):
                value = 0j
                for n in range(8):
                    k = ms * SAMPLES_MS + 8*h + n
                    raw = (data[k//2] >> (4*(k%2))) & 15
                    value += complex(levels[raw >> 2], levels[raw & 3]) * (-1j)**(n%4)
                encoded = (packed[ms*COMPACT_BYTES + h//4] >> (2*(h%4))) & 3
                self.assertEqual(encoded, (value.real < 0) + 2*(value.imag < 0))
            self.assertEqual(packed[(ms+1)*COMPACT_BYTES - 1] & 0xf0, 0)
        self.assertEqual(pack_block(bytes(BYTES_MS)), bytes(COMPACT_BYTES))

    def test_all_signs_and_replication(self):
        # 0xe4 holds sign pairs 0,1,2,3 in chronological order.
        compact = bytes([0xe4]) * (COMPACT_BYTES-1) + bytes([4])
        raw = expand_block(compact)
        self.assertEqual(len(raw), BYTES_MS)
        for h in range(HALF_CHIPS):
            z = complex(-1 if h & 1 else 1, -1 if h & 2 else 1)
            for phase in range(8):
                y = z * (1j)**(phase%4)
                i = 8*h + phase
                nibble = (raw[i//2] >> (4*(i%2))) & 15
                self.assertEqual(nibble, (8 if y.real < 0 else 0) + (2 if y.imag < 0 else 0))
        self.assertEqual(pack_block(raw), compact)

    def test_invalid_file_and_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, out = Path(tmp) / "samples", Path(tmp) / "output"
            for payload in (b"", bytes(COMPACT_BYTES-1), bytes(COMPACT_BYTES-1)+b"\xf0"):
                source.write_bytes(payload)
                with self.assertRaises(ValueError):
                    convert("expand", source, out)
                self.assertFalse(out.exists())
            for origin, length in ((1, BYTES_MS), (0, BYTES_MS-1)):
                source.write_bytes(HEADER.pack(b"EIQ1", RATE, IF_HZ, 1, origin, SAMPLES_MS) + bytes(length))
                with self.assertRaises(ValueError):
                    convert("pack", source, out)
                self.assertFalse(out.exists())
            source.write_bytes(bytes(COMPACT_BYTES))
            self.assertEqual(convert("expand", source, out), 1)
            previous = out.read_bytes()
            with self.assertRaises(FileExistsError):
                convert("expand", source, out)
            self.assertEqual(out.read_bytes(), previous)


class FakePort:
    def __init__(self, defect=None, used=4):
        self.defect = defect
        self.used = used
        self.lines = queue.Queue()
        self.received = bytearray()
        self.epochs = 600
        self.closed = False

    def reset_input_buffer(self):
        pass

    def read_until(self, suffix, maximum):
        return self.lines.get(timeout=2).encode() + suffix

    def write(self, chunk):
        if chunk.startswith(b"clocked "):
            command, epochs, expected = chunk.decode().split()
            self.assert_command = (command, int(epochs), int(expected, 16))
            self.lines.put("READY CLK1")
            self.lines.put("H " + FIELDS)
            return len(chunk)
        self.received.extend(chunk)
        if len(self.received) == self.epochs*COMPACT_BYTES:
            for n in (1, 2, 3):
                sample = (n + (self.defect == "sample" and n == 2)) * (RATE//5)
                mask = 1 if n == 1 else (1 << self.used) - 1
                row = [n, sample, 0, mask, mask, mask if n == 3 else 0, int(n == 3), 1, 0,
                       1, 2, 3, 0, 0, 0, 0, 0, self.used if n == 3 else 0, 1]
                time_us = 200000*n + (10000 if self.defect == "cadence" and n == 2 else 0)
                self.lines.put(f"S {time_us} " + ",".join(map(str, row)))
            crc = zlib.crc32(self.received) ^ (self.defect == "crc")
            self.lines.put(f"PASS epochs=600 elapsed_us=600010 late_us=6 starved=0 peak_backlog=10 crc={crc:08x}")
        return len(chunk)


class ClockedSerial(unittest.TestCase):
    def test_complete_stream_and_status(self):
        with tempfile.TemporaryFile() as f:
            samples = bytes(COMPACT_BYTES) * 600
            f.write(samples)
            f.seek(0)
            port, out = FakePort(), io.StringIO()
            result = run(port, f, out)
            self.assertEqual(port.assert_command, ("clocked", 600, zlib.crc32(samples)))
            self.assertEqual(bytes(port.received), samples)
            self.assertEqual(len(out.getvalue().splitlines()), 4)
            self.assertEqual(out.getvalue().splitlines()[0], FIELDS)
            self.assertEqual(result["valid_rows"], 1)
            self.assertEqual(result["events"]["first_lock"]["device_s"], .2)
            self.assertEqual(result["events"]["four_locks"]["sample_s"], .4)
            self.assertEqual(result["events"]["first_valid"]["device_s"], .6)
            self.assertIsNone(result["events"]["six_used"])
            self.assertIsNone(result["events"]["seven_used"])

    def test_usable_requires_a_fix(self):
        for used in (6, 7):
            with self.subTest(used=used), tempfile.TemporaryFile() as f:
                f.write(bytes(COMPACT_BYTES) * 600)
                f.seek(0)
                events = run(FakePort(used=used), f, io.StringIO())["events"]
                self.assertEqual(events["six_used"], events["first_valid"])
                self.assertEqual(events["seven_used"], events["first_valid"] if used == 7 else None)

    def test_corrupt_status_or_device_checksum(self):
        for defect in ("sample", "cadence", "crc"):
            with tempfile.TemporaryFile() as f:
                f.write(bytes(COMPACT_BYTES) * 600)
                f.seek(0)
                with self.assertRaises(ValueError):
                    run(FakePort(defect), f, io.StringIO())


if __name__ == "__main__":
    unittest.main()
