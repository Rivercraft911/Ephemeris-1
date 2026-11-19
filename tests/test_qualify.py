"""Exercise the physical RF evidence format with a small, explicitly artificial bench archive."""

import csv
import json
import io
from types import SimpleNamespace
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import zipfile

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from campaign import EARTH_RADIUS, FS, TRUTH_HEADER, XYZ, circular
from skydel_truth import convert
import qualify as q


class Evidence(unittest.TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        base = Path(tmp.name)
        name = self.name = "leo-550-2"
        cases = {name: q.CASES[name]}
        mask = patch.dict(q.CASES, cases, clear=True)
        mask.start()
        self.addCleanup(mask.stop)
        root = self.root = base / "run"
        self.case = root / name
        setup = {k: "TEST " + k for k in q.SETUP_FIELDS}
        self.serial = setup["pico_usb_serial"] = "TESTUSB"
        q.write_json(base / "setup", setup)
        (base / "image").write_bytes(b"test image")
        q.prepare(root, base / "image", base / "setup")
        p = q.plan(name)
        t = np.arange(p["duration_s"] * 5 + 1) / 5
        states = circular(
            EARTH_RADIUS + 550000,
            np.deg2rad(p["inclination_deg"]),
            np.deg2rad(p["raan_deg"]),
            np.deg2rad(p["phase_deg"]),
            t,
        )
        # A moving synthetic sky with four low elevations at right angles, one
        # high satellite and two intermediate ones. Ground truth stays independent.
        mapping = {
            "columns": {k: k for k in [*XYZ, "gps_week", "tow_s", "cn0_dbhz"]},
            "receiver": "rx.csv",
            "preset": "scenario",
            "run_log": "runlog",
            "satellites": [],
        }
        zenith = states[:, :3] / np.linalg.norm(states[:, :3], axis=1)[:, None]
        east = np.cross(np.array([0, 0, 1]), zenith)
        east /= np.linalg.norm(east, axis=1)[:, None]
        north = np.cross(zenith, east)
        with zipfile.ZipFile(self.case / "simulator.zip", "w") as z:
            z.writestr("scenario", b"test settings")
            z.writestr("runlog", b"test execution log")

            def export(name, column_names, data):
                stream = io.StringIO()
                w = csv.writer(stream)
                w.writerow(["gps_week", "tow_s", *column_names])
                for sec, row in zip(t, data):
                    w.writerow([p["gps_week"], p["tow_s"] + sec, *row])
                z.writestr(name, stream.getvalue())

            export("rx.csv", XYZ, states)
            for prn, (az, el) in enumerate(
                [(0, 18), (90, 18), (180, 18), (270, 18), (45, 55), (225, 55), (0, 80)],
                1,
            ):
                a, b = np.deg2rad([az, el])
                los = (
                    np.cos(b) * (np.sin(a) * east + np.cos(a) * north)
                    + np.sin(b) * zenith
                )
                pos = states[:, :3] + 2.1e7 * los
                filename = f"G{prn}.csv"
                export(
                    filename,
                    [*XYZ[:3], "cn0_dbhz"],
                    np.column_stack((pos, np.full(len(t), 45))),
                )
                mapping["satellites"].append(dict(prn=prn, file=filename, healthy=True))
            z.writestr("truth-map.json", json.dumps(mapping))
        convert(self.case / "simulator.zip", self.case / "truth.csv", p)
        with (self.case / "receiver.csv").open("w") as rx:
            rx.write("#usb_serial=" + self.serial + "\n")
            rows = csv.writer(rx)
            rows.writerow(["sample", "gaps", "valid", "week_mod1024", "tow_s", *XYZ])
            for sec, xyz in zip(t, states):
                tow = p["tow_s"] + sec
                rows.writerow([round(sec * FS), 0, 1, p["gps_week"] % 1024, tow, *xyz])
            rx.write(
                "#stats blocks=150000 missed=0 fifo_stalls=0 clock_timeouts=0 dropped_usb_lines=0 clock_alive=1 warm_restored=0\n"
            )
        self.timing = self.case / "timing.csv"
        self.timing.write_text(f"sample,elapsed_s\n0,0\n{150*FS},150\n")

    def test_sealed_archive(self):
        self.assertFalse(q.qualify(self.root)["passed"])
        q.seal(self.root, self.name, 0)
        report = q.qualify(self.root)
        self.assertTrue(report["passed"], report)
        with self.assertRaisesRegex(ValueError, "already sealed"):
            q.seal(self.root, self.name, 0)
        self.timing.write_text(self.timing.read_text() + "\n")
        self.assertIn("files changed", q.qualify(self.root)["errors"][0])
        (self.root / "firmware.uf2").write_bytes(b"other build")
        self.assertIn("firmware", q.qualify(self.root)["errors"][0])

    def test_setup_consistency(self):
        path = self.root / "campaign.json"
        meta = json.loads(path.read_text())
        meta["equipment"]["rf_board_id"] += "changed"
        q.write_json(path, meta)
        with self.assertRaisesRegex(ValueError, "campaign identity"):
            q.seal(self.root, self.name, 0)

    def test_physical_counters(self):
        path = self.case / "receiver.csv"
        path.write_text(
            path.read_text()
            .replace("fifo_stalls=0", "fifo_stalls=3")
            .replace("warm_restored=0", "warm_restored=1")
        )
        q.seal(self.root, self.name, 2)
        result = q.qualify(self.root)["runs"][0]["result"]
        self.assertFalse(result["passed"])
        self.assertEqual(
            set(result["faults"]),
            {"Pico fifo_stalls", "Pico warm_restored", "simulator RF underruns"},
        )

    def test_unrelated_receiver(self):
        path = self.case / "receiver.csv"
        path.write_text(path.read_text().replace("TESTUSB", "another unit"))
        q.seal(self.root, self.name, 0)
        self.assertIn("USB serial", q.qualify(self.root)["errors"][0])

    def test_wrong_simulator_epoch(self):
        path = self.case / "truth.csv"
        raw = path.read_text().replace("194417.2", "194417.21", 1)
        path.write_text(raw)
        q.seal(self.root, self.name, 0)
        self.assertIn("GPS epoch", q.qualify(self.root)["errors"][0])

    def test_collector(self):
        device = io.BytesIO()
        p = self.case / "receiver.csv"
        p.unlink()
        replies = iter(
            [
                b"sequence,sample,gaps,valid\n",
                f"1,{150*FS},0,0\n".encode(),
                b"#stats blocks=150000\n",
            ]
        )

        class Port:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                pass

            def readline(self):
                return next(replies)

            def write(self, raw):
                device.write(raw)

        serial = SimpleNamespace(Serial=lambda *a, **kw: Port())
        ports = SimpleNamespace(
            comports=lambda: [SimpleNamespace(serial_number=self.serial, device="TEST")]
        )
        with patch.dict(
            sys.modules,
            {"serial": serial, "serial.tools": SimpleNamespace(list_ports=ports)},
        ):
            q.collect(self.root, self.name)
        self.assertEqual(device.getvalue(), b"stats\n")
        self.assertTrue(
            p.read_text().startswith("#usb_serial=TESTUSB\nsequence,sample,")
        )
        self.assertTrue(p.read_text().endswith("#stats blocks=150000\n"))

    def test_skydel_raw_import(self):
        from skydel_truth import convert

        caller = dict(gps_week=2431, tow_s=604799.8, duration_s=1)
        mapping = {
            "columns": {k: k for k in [*XYZ, "gps_week", "tow_s", "relative_db"]},
            "receiver": "raw/rx.csv",
            "preset": "scenario",
            "run_log": "runlog",
            "satellites": [],
        }
        offsets = np.array(
            [[1, 1, 1], [1, 1, -1], [1, -1, 1], [1, -1, -1], [1, 0, 0], [-1, 2, 0]]
        )
        # One SV has a different elevation, one signal is below mask, and a
        # seventh is healthy but faint. Only the first five count.
        rf_radius = 6_900_000
        epochs = [
            (2431 + (604799.8 + i / 5 >= 604800), (604799.8 + i / 5) % 604800)
            for i in range(6)
        ]
        archive = self.case / "raw-test.zip"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("scenario", b"settings")
            z.writestr("runlog", b"logs")
            rx = io.StringIO()
            w = csv.writer(rx)
            w.writerow(["gps_week", "tow_s", *XYZ])
            for week, tow in epochs:
                w.writerow([week, tow, rf_radius, 0, 0, 0, 0, 0])
            z.writestr(mapping["receiver"], rx.getvalue())
            for j, off in enumerate([*offsets, offsets[0]]):
                prn = j + 1
                name = f"raw/G{prn}.csv"
                mapping["satellites"].append(
                    dict(prn=prn, file=name, healthy=True, reference_cn0_dbhz=45)
                )
                sv = io.StringIO()
                w = csv.writer(sv)
                w.writerow(["gps_week", "tow_s", *XYZ[:3], "relative_db"])
                for week, tow in epochs:
                    w.writerow(
                        [
                            week,
                            tow,
                            *(np.array([rf_radius, 0, 0]) + 2e7 * off),
                            -15 if prn == 7 else 0,
                        ]
                    )
                z.writestr(name, sv.getvalue())
            z.writestr("truth-map.json", json.dumps(mapping))
        out = self.case / "import.csv"
        convert(archive, out, caller)
        rows = q.table(out)
        self.assertEqual(len(rows), 6)
        self.assertTrue(all(int(r["healthy_sv"]) == 5 for r in rows))
        directions = offsets[:5] / np.linalg.norm(offsets[:5], axis=1)[:, None]
        design = np.column_stack((directions, np.ones(5)))
        pdop = np.sqrt(np.trace(np.linalg.inv(design.T @ design)[:3, :3]))
        self.assertAlmostEqual(float(rows[0]["pdop"]), pdop)
        self.assertEqual(int(rows[-1]["gps_week"]), 2432)

    def test_timebase(self):
        rows = [
            dict(sample=0, elapsed_s=0.009),
            dict(sample=16000000, elapsed_s=0.009 + 16000000 / (FS * 1.000002)),
        ]
        zero, rate, residual = q.timebase(rows)
        self.assertAlmostEqual(zero, 0.009)
        self.assertAlmostEqual(rate, FS * 1.000002)
        self.assertLess(residual, 1e-12)
        with self.assertRaises(ValueError):
            q.timebase(rows[::-1])


if __name__ == "__main__":
    unittest.main()
