"""Check coordinate conversion and the laptop display against the Pico wire protocol."""

import io
import math
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from ground_fix import Display, geodetic, score_site, watch
from iqio import RATE


HEADER = ("sequence,sample,gaps,active_mask,locked_mask,nav_mask,valid,week_mod1024,tow_s,"
          "x_m,y_m,z_m,vx_mps,vy_mps,vz_mps,bias_m,drift_mps,sv,pdop")
STEP = RATE // 5


def record(sequence, sample=None, gaps=0, valid=1, x=6378137):
    sample = sequence * STEP if sample is None else sample
    return f"{sequence},{sample},{gaps},15,15,15,{valid},142,525609,{x},2,3,0,0,0,0,0,4,2.1"


class GroundFix(unittest.TestCase):
    def test_geodetic(self):
        for xyz, llh in [
            ((6378137, 0, 0), (0, 0, 0)),
            ((0, -6378137 + 37, 0), (0, -90, -37)),
            ((0, 0, 6356752.314245179 + 25), (90, 0, 25)),
            ((-3194419.1450605746, -3194419.1450605746, -4487348.408865919),
             (-45, -135, 0)),
        ]:
            for found, expected in zip(geodetic(*xyz), llh):
                self.assertAlmostEqual(found, expected, places=6)
        for xyz in [(0, 0, 0), (math.nan, 1, 1)]:
            with self.assertRaises(ValueError):
                geodetic(*xyz)

    def test_freshness(self):
        view = Display()
        view.update(HEADER, 0)
        self.assertIn("lat ", view.update(record(5), 1))
        self.assertIsNone(view.update("#stats clock_alive=1", 1.5))
        self.assertIsNone(view.expire(1.8))
        self.assertIn("fresh", view.expire(2))
        self.assertIsNone(view.position)
        self.assertIn("stale", view.update(record(4), 3))
        self.assertIn("stale", view.update(record(5), 4))
        self.assertIn("stopped", view.update(record(6, sample=5*STEP), 5))
        self.assertIn("discontinuity", view.update(record(7, gaps=1), 6))
        self.assertIsNone(view.position)
        self.assertIn("lat ", view.update(record(8, gaps=1), 7))
        self.assertIn("Searching", view.update(record(9, gaps=1, valid=0), 8))
        self.assertIsNone(view.position)
        self.assertIn("invalid", view.update(record(10, gaps=1, x="nan"), 9))
        self.assertIsNone(view.position)
        view.update(HEADER, 10)
        self.assertIn("lat ", view.update(record(1), 11))

    def test_serial_partial_timeout_and_unplug(self):
        chunks = iter([(HEADER + "\n").encode(), record(1)[:10].encode(), b"",
                       (record(1)[10:] + "\n").encode(), b"#stats\n", b"", b""])

        class Port:
            def read_until(self, suffix, limit):
                try:
                    return next(chunks)
                except StopIteration:
                    raise OSError("unplugged")

        view, statuses, log = Display(), [], io.BytesIO()
        times = iter([0, .01, .2, .21, .4, .8, 1.3])
        with self.assertRaises(OSError):
            watch(Port(), view, lambda s: statuses.append(s) if s else None,
                  log, lambda: next(times))
        self.assertIn("lat ", statuses[1])
        self.assertEqual(statuses[-2:], ["No fix | no fresh receiver status", "No fix | USB disconnected"])
        self.assertIsNone(view.position)
        self.assertEqual(log.getvalue().decode(), HEADER + "\n" + record(1) + "\n#stats\n")

    def test_known_site(self):
        lines = [HEADER] + [record(n) for n in (1, 2, 4, 5, 6)]
        lines += [record(7, valid=0), record(8), record(9, x=6378237)]
        result = score_site(lines, (0, 0, 0))
        self.assertEqual(result["first_sample_s"], .2)
        self.assertEqual(result["longest"], 3)
        self.assertEqual(result["fixes"], 7)
        self.assertAlmostEqual(result["p95"][0], math.hypot(2, 3))
        self.assertGreater(result["p95"][1], 20)


if __name__ == "__main__":
    unittest.main()
