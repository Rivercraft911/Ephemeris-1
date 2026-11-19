"""Check signed height and common observation scoring at a known equatorial site."""

from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from score_antenna import fix_errors, score


class AntennaScore(unittest.TestCase):
    def test_signed_up_and_common_epochs(self):
        site = np.array([6378137., 0., 0.])
        a = dict(sample='3273600', valid='1', x_m='6378134', y_m='4', z_m='0', nav_mask='15', sv='4')
        b = dict(a, sample='6547200', x_m='6378144', nav_mask='31', sv='5')
        baseline = fix_errors([a, b], site)
        trial = fix_errors([dict(a, x_m='6378136'), dict(b, nav_mask='47'), dict(b, valid='0')], site)
        result = score(baseline, trial)
        self.assertEqual(result['trial']['fixes'], 2)
        self.assertEqual(result['paired_trial']['fixes'], 1)
        self.assertEqual(result['paired_trial']['median_up_m'], -1)
        self.assertEqual(result['paired_baseline']['p95_horizontal_m'], 4)
        self.assertEqual(result['paired_baseline']['p95_3d_m'], 5)
        self.assertEqual(score({}, {})['paired_trial'], {'fixes': 0})


if __name__ == '__main__':
    unittest.main()
