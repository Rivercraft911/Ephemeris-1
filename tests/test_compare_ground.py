"""Check receiver-to-receiver alignment and local WGS 84 axes."""

from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compare_ground import compare, match, reference_tracks
from ground_fix import A
from iqio import RATE


def reference(t, height=0):
    # Equator, 90 degrees east. The local up axis is ECEF +Y.
    return dict(gps_week=1734, tow_s=t, x_m=0, y_m=A + height, z_m=0)


def receiver(t, xyz):
    return dict(valid="1", sample=RATE, week_mod1024=710, tow_s=t,
                x_m=xyz[0], y_m=xyz[1], z_m=xyz[2])


class CompareGround(unittest.TestCase):
    def test_local_frame_at_interpolated_epoch(self):
        refs = [reference(100, 0), reference(100.1, 2)]
        fix = receiver(100.05, (-3, A + 1 + 12, 4))
        summary, points = compare([dict(valid="0"), fix], refs)
        self.assertEqual(summary["receiver_rows"], 2)
        self.assertEqual(summary["matched_fixes"], 1)
        self.assertEqual(summary["first_fix"]["elapsed_s"], 1)
        self.assertAlmostEqual(summary["first_fix"]["lon_deg"], 90, places=3)
        np.testing.assert_allclose(points[0], [1, 5, 12, 13], atol=1e-8)
        self.assertAlmostEqual(summary["separation"]["distance_3d_m"]["p95"], 13)

    def test_gap_bounds_and_week(self):
        refs = [reference(100), reference(100.1), reference(100.3), reference(100.4)]
        track = reference_tracks(refs)[710]
        for t in (99.99, 100.2, 100.41):
            self.assertIsNone(match(track, t))
        for t in (100, 100.05, 100.1, 100.3, 100.35, 100.4):
            np.testing.assert_array_equal(match(track, t), (0, A, 0))
        row = receiver(100.05, (0, A, 0))
        summary, _ = compare([dict(row, week_mod1024=711)], refs)
        self.assertEqual(summary["valid_fixes"], 1)
        self.assertEqual(summary["matched_fixes"], 0)
        self.assertEqual(summary["separation"], {})
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            reference_tracks([refs[0], dict(refs[0], gps_week=2758)])


if __name__ == "__main__":
    unittest.main()
