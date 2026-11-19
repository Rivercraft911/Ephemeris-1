"""Independent scorer negative controls (SPDX-License-Identifier: Apache-2.0)."""

from pathlib import Path
import sys
import unittest
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from campaign import FS, Q, SPEC, TRUTH_HEADER, XYZ, geometry, orbit_period, score


from qualify import qualify
from stress import Stress


class Campaign(unittest.TestCase):
    def setUp(self):
        self.truth = []
        self.rows = []
        for i in range(60):
            t = i / 5
            r = dict(zip(XYZ, (6_900_000.0, t * 7450, 0, 0, 7450.0, 0)))
            self.truth.append(dict(t_s=t, healthy_sv=8, pdop=1.9, **r))
            self.rows.append(dict(sample=i * FS // 5, gaps=0, valid="1", **r))

    def test_incomplete_campaign_cannot_qualify(self):
        import tempfile

        with tempfile.TemporaryDirectory() as path:
            report = qualify(Path(path))
        self.assertFalse(report["passed"])
        self.assertTrue(report["errors"])

    def test_rf_stress(self):
        prns = np.arange(1, 7)
        elev = np.full(6, np.pi / 4)
        a = np.arange(1.0, 7.0)
        self.assertTrue(
            np.array_equal(Stress("total-outage").amplitudes(120, prns, elev, a), a)
        )
        self.assertFalse(Stress("total-outage").amplitudes(131, prns, elev, a).any())
        self.assertEqual(
            np.count_nonzero(Stress("three-visible").amplitudes(180, prns, elev, a)), 3
        )
        faded = Stress("single-fade").amplitudes(131, prns, elev, a)
        self.assertAlmostEqual(faded[-1] / a[-1], 10 ** (-24 / 20))
        self.assertAlmostEqual(Stress("frequency-high").clock_scale, 1.000002)
        self.assertTrue(
            np.all(
                Stress("antenna-tumble").amplitudes(145, prns, elev, a, np.zeros(6)) < a
            )
        )

    def test_orbits(self):
        for km in SPEC["altitudes_km"]:
            t = np.array([0, 0.01, orbit_period(km) / 2])
            x, count, dop = geometry(km, SPEC["starts"][0], t)
            self.assertAlmostEqual(
                np.linalg.norm(x[0, :3]), 6378137 + km * 1000, places=5
            )
            self.assertTrue(
                np.allclose((x[1, :3] - x[0, :3]) / 0.01, x[0, 3:], atol=0.1)
            )
            self.assertTrue(np.isfinite(dop).all())
            self.assertTrue((count >= 4).all())
        self.assertEqual(len(SPEC["starts"]) * len(SPEC["altitudes_km"]), 12)

    def test_truth_and_corruption(self):
        self.assertTrue(score(self.truth, self.rows, 0)["passed"])
        # A 2 ms timing offset consumes ~15 m on this trajectory.
        self.assertFalse(score(self.truth, self.rows, 0.002)["passed"])
        for name, value in (("z_m", 60), ("vz_mps", 1), ("gaps", 1), ("valid", "0")):
            rows = [dict(r, **{name: value}) for r in self.rows]
            self.assertFalse(score(self.truth, rows, 0)["passed"], name)
        self.assertFalse(score(self.truth, self.rows[::2], 0)["passed"])
        self.assertFalse(score(self.truth, [*self.rows, self.rows[-1]], 0)["passed"])
        self.assertFalse(
            score(
                self.truth,
                [dict(r, sample=self.rows[0]["sample"]) for r in self.rows],
                0,
            )["passed"]
        )
        self.truth[10]["healthy_sv"] = 3
        self.assertFalse(score(self.truth, self.rows, 0)["passed"])


if __name__ == "__main__":
    unittest.main()
