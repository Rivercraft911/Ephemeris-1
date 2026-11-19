"""Host-side wire order, PRBS phase search and fault checks. SPDX-License-Identifier: Apache-2.0"""
import io
import os
import re
import subprocess
from pathlib import Path
import tempfile
import unittest

from bench import run, verify_pattern
from iqio import write_capture


def fixture_bytes():
    # Literal Galois update used in the independent Pico transmitter, including SRAM ring wrap.
    state = 1
    cycle = bytearray()
    for _ in range(65536):
        cycle.append(state & 15)
        for _ in range(4):
            state = (state >> 1) ^ (0xB400 if state & 1 else 0)
    start = 65529
    samples = (cycle * 4)[start:start + 163680]
    return bytes(lo | hi << 4 for lo, hi in zip(samples[::2], samples[1::2]))


class Port(io.BytesIO):
    def reset_input_buffer(self):
        pass

    def write(self, command):
        if command != b"capture\n":
            raise ValueError("unexpected command")
        return len(command)


class TestBench(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.raw = fixture_bytes()

    def test_configuration_bus_frames(self):
        firmware = Path(__file__).resolve().parent
        design = (firmware.parent / "docs" / "design.md").read_text()
        fields = re.findall(r"^\| (\d) \| \w+ \| `0x([0-9A-Fa-f]{7})` \|", design, re.M)
        self.assertEqual([int(addr) for addr, _ in fields], list(range(10)))
        expected = b"".join(((int(data, 16) << 4) | int(addr)).to_bytes(4, "big")
                            for addr, data in fields)
        # Exercise the actual freestanding C++ serializer used for SPI writes.
        source = r'''#include "registers.hpp"
#include <cstdio>
int main() {
    for (unsigned i = 0; i < board::register_data.size(); ++i) {
        auto bytes = board::configuration_bytes(i);
        std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    }
}
'''
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / "frames"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-x", "c++",
                            "-I", str(firmware), "-o", str(exe), "-"], input=source,
                           text=True, check=True)
            self.assertEqual(subprocess.check_output([str(exe)]), expected)

    def test_independent_pattern(self):
        self.assertEqual(verify_pattern(self.raw), 65529)
        damaged = bytearray(self.raw)
        damaged[40000] ^= 0x20
        with self.assertRaisesRegex(ValueError, "sample 80001"):
            verify_pattern(damaged)

    def test_save_complete_frame(self):
        frame = io.BytesIO()
        write_capture(frame, self.raw)
        with tempfile.TemporaryDirectory() as root:
            dst = Path(root) / "record.eiq"
            run(Port(frame.getvalue()), "capture", dst, False)
            self.assertEqual(dst.read_bytes(), frame.getvalue())
            with self.assertRaises(ValueError):
                run(Port(frame.getvalue()[:-1]), "capture", dst, False)
            self.assertEqual(dst.read_bytes(), frame.getvalue())


if __name__ == "__main__":
    unittest.main()
