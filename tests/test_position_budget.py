"""Independent controls for matched positions and the range-to-position projection."""
import csv
from pathlib import Path
import tempfile
import unittest

import numpy as np

from independent_orbit import compare_positions, rate_trial, score_smoothing, trajectory
from position_budget import C, FS, score


class PositionBudget(unittest.TestCase):
    def test_rate_trial_compares_current_positions_across_a_gap(self):
        def motion(t):
            zero = np.zeros_like(t)
            return np.column_stack((t*t, zero, zero, 2*t, zero, zero))

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "rate.csv"
            with path.open("w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(("sample", "valid", "gaps", "x_m", "y_m", "z_m", "vx_mps", "vy_mps", "vz_mps"))
                for i in range(1, 31):
                    writer.writerow((i * (FS // 5), int(i != 10), int(i >= 10), *motion(np.array([i / 5]))[0]))
            short, = rate_trial(path, motion, 6)
            self.assertEqual((short["epochs_1hz"], short["epochs_5hz"]), (5, 21))
            self.assertLess(short["filtered_5hz_p95_m"], 1e-12)

    def test_paired_smoothing_scores_warmup_resets_and_device_output(self):
        def motion(t):
            zero = np.zeros_like(t)
            return np.column_stack((10*t*t, zero, zero, 20*t, zero, zero))

        fields = ("sample", "valid", "gaps", "x_m", "y_m", "z_m", "vx_mps", "vy_mps", "vz_mps",
                  "raw_x_m", "raw_y_m", "raw_z_m", "motion_samples")
        rows, recent = [], []
        for i in range(15):
            # A new gap at fix 11 resets the filter even though that fix is valid.
            good = i != 6
            if not good or i == 10:
                recent.clear()
            t = (i + 1) / 5
            pos, _, _, vel, _, _ = motion(np.array([t]))[0]
            error = (i % 5) - 2
            if good:
                recent = (recent + [error])[-4:]
            output_x = pos + np.mean(recent) if good else np.nan
            rows.append([(i+1) * (FS // 5), int(good), int(i >= 10), output_x, 0, 0,
                         vel, 0, 0, pos + error, 0, 0, len(recent)])

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "paired.csv"
            def write():
                with path.open("w", newline="") as f:
                    writer = csv.writer(f)
                    writer.writerow(fields)
                    writer.writerows(rows)
            write()
            result = score_smoothing(path, motion, 3, 4)
            self.assertTrue(result["matches_reference"])
            self.assertLess(result["max_reference_difference_3d_m"], 1e-10)
            self.assertEqual((result["all_valid"]["epochs"], result["full_window"]["epochs"]), (14, 5))
            self.assertEqual((result["first_fix_s"], result["full_window"]["first_s"]), (0.2, 0.8))
            self.assertEqual(result["first_onboard_error_m"], result["first_raw_error_m"])
            rows[1][3] += 0.1
            write()
            self.assertFalse(score_smoothing(path, motion, 3, 4)["matches_reference"])
            rows[10][-1] = 4
            write()
            with self.assertRaisesRegex(ValueError, "motion_samples"):
                score_smoothing(path, motion, 3, 4)

    def test_pairs_require_the_same_satellites(self):
        truth = trajectory(np.arange(1, 6) / 5)
        with tempfile.TemporaryDirectory() as tmp:
            ref, test = (Path(tmp) / name for name in ("one.csv", "two.csv"))

            def write(path, offset, last_mask):
                with path.open("w", newline="") as f:
                    writer = csv.writer(f)
                    writer.writerow(("sample", "valid", "nav_mask", "sv", "x_m", "y_m", "z_m"))
                    for n, xyz in enumerate(truth[:, :3], 1):
                        writer.writerow((n * (FS // 5), 1, last_mask if n == 5 else 15, 4,
                                         xyz[0] + offset, xyz[1], xyz[2]))

            write(ref, 0, 15)
            write(test, 2, 15)
            result = compare_positions(ref, test, trajectory, 1)
            self.assertEqual(result["paired_fixes"], 5)
            self.assertAlmostEqual(result["candidate_p95_m"], 2)
            write(test, 2, 23)
            with self.assertRaisesRegex(ValueError, "different satellites"):
                compare_positions(ref, test, trajectory, 1)

    def test_range_projection_removes_common_clock(self):
        t = 1.0
        xyz = trajectory(np.array([t]))[0, :3]
        directions = np.array([[1, 1, 1], [1, -1, -1], [-1, 1, -1], [-1, -1, 1]]) / np.sqrt(3)
        delta = np.array([1., -2., 0.5])
        radius = 22e6
        # A spatial offset in the recovered code timing plus a common receiver clock error.
        timing = directions @ delta
        tow = 525609 + t
        trace = []
        for prn, (direction, dt) in enumerate(zip(directions, timing), 1):
            trace.append([prn, tow, tow - (radius + dt) / C, radius + dt + 75000,
                          *(xyz - radius * direction), *(xyz + delta), 1.5])
        ranges = [(10, prn, radius) for prn in range(1, 5)]
        status = np.zeros(825, dtype=[("sample", "i8"), ("valid", "i1"), ("tow_s", "f8"),
                                      ("sv", "i1"), ("nav_mask", "i8")])
        status["sample"] = np.arange(1, 826) * (FS // 5)
        status[4] = (FS, 1, tow, 4, 15)
        budget = score(np.array(trace), ranges, status, 550, 12)
        self.assertLess(budget["max_linear_reconstruction_error_m"], 1e-6)
        self.assertAlmostEqual(budget["full"]["p95_from_code_timing_m"], np.linalg.norm(delta), delta=.03)
        self.assertLess(budget["full"]["p95_from_range_model_m"], .03)


if __name__ == "__main__":
    unittest.main()
