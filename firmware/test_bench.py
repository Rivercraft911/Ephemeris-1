"""Host-side wire order, PRBS phase search and fault checks. SPDX-License-Identifier: Apache-2.0"""
import io
from contextlib import redirect_stdout
import os
import re
import subprocess
from pathlib import Path
import tempfile
import unittest

from bench import blind_lock, run, verify_pattern
from iqio import BYTES_MS, SAMPLES_MS, write_capture


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
    def __init__(self, response, expected_command=b"capture\n"):
        super().__init__(response)
        self.expected_command = expected_command

    def reset_input_buffer(self):
        pass

    def write(self, command):
        if command != self.expected_command:
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
        with tempfile.TemporaryDirectory() as root, redirect_stdout(io.StringIO()):
            dst = Path(root) / "record.eiq"
            run(Port(frame.getvalue()), "capture", dst, False)
            self.assertEqual(dst.read_bytes(), frame.getvalue())
            with self.assertRaises(ValueError):
                run(Port(frame.getvalue()[:-1]), "capture", dst, False)
            self.assertEqual(dst.read_bytes(), frame.getvalue())

    def test_kernel_reports(self):
        timing = b"PASS samples=163680 mismatches=0 clock=internal epochs=10\n"
        coarse = b"MEASURED samples=163680 errors=0 clock=external worst_margin_us=10,20,30\n"
        with redirect_stdout(io.StringIO()):
            run(Port(timing, b"selftiming\n"), "timing", None, False, internal=True)
            run(Port(coarse, b"coarse\n"), "coarse", None, False)
            with self.assertRaisesRegex(ValueError, "coarse"):
                run(Port(coarse.replace(b"errors=0", b"errors=1"), b"coarse\n"), "coarse", None, False)

    def test_blind_acquisition(self):
        class BlindPort(io.BytesIO):
            def reset_input_buffer(self):
                self.writes = []

            def write(self, data):
                self.writes.append(data)
                return len(data)

        record = (f"PASS prn=4 consecutive=20 lock_sample={400*SAMPLES_MS} lock_us=400071 "
                  f"end_sample={419*SAMPLES_MS} elapsed_us=419077 phase_chips=137.3 "
                  "doppler_hz=-2 capture_gaps=0 missed=0 stalls=0 clock_timeouts=0\n").encode()
        with redirect_stdout(io.StringIO()):
            def reply(report):
                return BlindPort(b"READY 8184\n" + report)
            port = reply(record)
            blind_lock(port, 4, 60)
            self.assertEqual(port.writes[0], b"blind 60\n")
            self.assertEqual(len(port.writes[1]), BYTES_MS)
            self.assertEqual(len(port.writes), 2)
            for bad in [b"FAIL epochs=60000\n", record.replace(b"prn=4", b"prn=1"),
                        record.replace(b"137.3", b"nan"), record.replace(b"stalls=0", b"stalls=1")]:
                with self.assertRaises(ValueError):
                    blind_lock(reply(bad), 4, 60)

    def test_axis_lookup(self):
        # Exhaust every possible eight-sample lane input in both positions,
        # including ties and byte-lane overflow. Compute the independent truth
        # one signed sample at a time with four explicit complex rotations.
        source = r'''#include "axis_capture.hpp"
#include <cassert>
#include <initializer_list>
int main() {
    const int level[] = {1,3,-1,-3};
    for (unsigned iw=0; iw<65536; ++iw) {
        for (unsigned mask : {0u, 1u, 0x5555u, 0xaaaau, 0xffffu}) {
            unsigned qw = ((iw * 25173u + 13849u) ^ mask) & 65535u;
            unsigned got = bench::combine_axes(bench::axis_partial(iw), bench::axis_partial(qw));
            int ri=0, rq=0;
            for (unsigned n=0; n<8; ++n) {
                int i=level[(iw>>(2*n))&3], q=level[(qw>>(2*n))&3];
                switch (n%4) {
                case 0: ri+=i; rq+=q; break;
                case 1: ri+=q; rq-=i; break;
                case 2: ri-=i; rq-=q; break;
                default: ri-=q; rq+=i; break;
                }
            }
            assert(bool(got&0x80)==(ri>=0) && bool(got&0x8000)==(rq>=0));
        }
    }
}
'''
        firmware = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / "axes"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-x", "c++",
                            "-I", str(firmware), "-o", str(exe), "-"],
                           input=source, text=True, check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
