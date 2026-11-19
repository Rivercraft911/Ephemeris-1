"""Check sign, sample rate and chunk joins for recorded L1 I/Q and real IF."""

from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from import_ground_rf import FORMATS, INPUT_MS, baseband


class GroundRF(unittest.TestCase):
    def test_carrier_through_block_boundaries(self):
        for name, (rate, if_hz) in FORMATS.items():
            with self.subTest(source=name):
                ms, tone = 5, -312375
                clock = np.arange(ms * (rate // 1000)) / rate
                z = 100 * np.exp(1j * (2 * np.pi * (if_hz + tone) * clock + .37))
                raw = z.real if if_hz else np.column_stack((z.real, z.imag))
                raw = raw.round().astype(np.int8)
                whole = np.empty(ms * INPUT_MS, np.complex64)
                split = np.empty_like(whole)
                baseband(raw, rate, if_hz, ms, whole, block_ms=ms)
                baseband(raw, rate, if_hz, ms, split, block_ms=1)
                np.testing.assert_array_equal(split, whole)
                phase = 2 * np.pi * tone * np.arange(len(whole)) / (INPUT_MS * 1000) + .37
                expected = (50 if if_hz else 100) * np.exp(1j * phase)
                self.assertLess(np.max(np.abs(whole[40:-40] - expected[40:-40])), 1)


if __name__ == '__main__':
    unittest.main()
