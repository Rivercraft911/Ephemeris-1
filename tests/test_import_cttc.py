"""Check timing, sign and quantization through the CTTC recording adapter."""

import io
from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from import_cttc import INPUT_MS, convert
from iqio import IF_HZ, RATE, SAMPLES_MS, read_capture
from vectors import pack


class ImportCTTC(unittest.TestCase):
    def test_chunk_boundaries(self):
        rng = np.random.default_rng(29453)
        iq = rng.integers(-8000, 8001, (5 * INPUT_MS, 2), dtype=np.int16)
        single, chunked = io.BytesIO(), io.BytesIO()
        convert(iq, single, block_ms=5)
        convert(iq, chunked, block_ms=2)
        self.assertEqual(single.getvalue(), chunked.getvalue())

    def test_complex_tone(self):
        # Nonintegral cycles per millisecond expose any phase reset at a join.
        tone_hz = -310375
        n = 4 * INPUT_MS
        phase = 0.207
        z = 8000 * np.exp(1j * (2 * np.pi * tone_hz * np.arange(n) / 4e6 + phase))
        iq = np.column_stack((z.real, z.imag)).round().astype(np.int16)
        out = io.BytesIO()
        threshold = convert(iq, out, block_ms=1)
        self.assertAlmostEqual(threshold, 8000 / np.sqrt(2), delta=0.03)
        out.seek(0)
        origin, actual = read_capture(out)
        self.assertEqual(origin, 0)
        t = np.arange(4 * SAMPLES_MS) / RATE
        ideal = pack(8000 / threshold * np.exp(1j * (2*np.pi*(IF_HZ + tone_hz)*t + phase)))
        # At file ends the resampler zero-pads its FIR. Interior differences
        # close to a quantization threshold include the FIR passband ripple.
        a, b = (np.frombuffer(x, np.uint8)[32:-32] for x in (actual, ideal))
        self.assertLess(np.mean(a != b), 0.002)

    def test_invalid_source(self):
        for n in (0, INPUT_MS - 1, INPUT_MS):
            with self.subTest(n=n), self.assertRaises(ValueError):
                convert(np.zeros((n, 2), dtype=np.int16), io.BytesIO())


if __name__ == "__main__":
    unittest.main()
