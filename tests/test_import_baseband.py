"""Check the receive-only PCM adapter independently of the GPS waveform generator."""

import io
from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from import_baseband import convert
from iqio import SAMPLES_MS, read_capture


class Baseband(unittest.TestCase):
    def test_positive_if(self):
        z = np.array([[50, 0]] * SAMPLES_MS, dtype=np.int8).tobytes()
        f = io.BytesIO()
        convert(io.BytesIO(z), f, SAMPLES_MS, 0.03, 0, 7)
        first, raw = read_capture(io.BytesIO(f.getvalue()))
        self.assertEqual(first, 0)
        self.assertEqual(raw, bytes.fromhex("14 3c") * (SAMPLES_MS // 4))

    def test_rejects_truncation(self):
        for n in (1, SAMPLES_MS):
            with self.assertRaises(ValueError):
                convert(io.BytesIO(b"\0" * 10), io.BytesIO(), n, 1, 0, 1)


if __name__ == "__main__":
    unittest.main()
