"""Convert archived Skydel raw logs into the independent 5 Hz conducted truth table."""

import csv
import io
import json
import math
import zipfile

import numpy as np

from campaign import Q, TRUTH_HEADER, XYZ


def measurements(archive_path, case):
    # Column names and calibrated levels are part of the simulator evidence.
    # Different Skydel releases and RF engines expose different power columns.
    with zipfile.ZipFile(archive_path) as archive:
        mapping = json.loads(archive.read("truth-map.json"))
        for field in ("preset", "run_log"):
            if archive.getinfo(mapping[field]).file_size < 1:
                raise ValueError(f"missing simulator {field}")
        names = mapping["columns"]
        hz = Q["hz"]
        count = case["duration_s"] * hz + 1

        def rows(path):
            with archive.open(path) as raw, io.TextIOWrapper(
                raw, encoding="utf-8-sig", newline=""
            ) as stream:
                yield from csv.DictReader(stream)

        def index(row):
            week = int(row[names["gps_week"]])
            tow = float(row[names["tow_s"]])
            elapsed = (week - case["gps_week"]) * 604800 + (tow - case["tow_s"])
            i = round(elapsed * hz)
            return i if 0 <= i < count and abs(elapsed - i / hz) < 1e-5 else None

        rx = np.full((count, 6), np.nan)
        weeks = np.zeros(count, int)
        times = np.zeros(count)
        for row in rows(mapping["receiver"]):
            i = index(row)
            if i is None:
                continue
            if np.isfinite(rx[i, 0]):
                raise ValueError("duplicate simulator receiver epoch")
            rx[i] = [float(row[names[k]]) for k in XYZ]
            weeks[i], times[i] = int(row[names["gps_week"]]), float(row[names["tow_s"]])
        if not np.isfinite(rx).all():
            raise ValueError("incomplete receiver trajectory from simulator")
        zenith = rx[:, :3] / np.linalg.norm(rx[:, :3], axis=1)[:, None]
        sine_mask = math.sin(math.radians(5))
        normals = np.zeros((count, 4, 4))
        visible = np.zeros(count, int)
        prns = set()
        for sv in mapping["satellites"]:
            prn = sv["prn"]
            if type(prn) is not int or not 1 <= prn <= 32 or prn in prns:
                raise ValueError("expected unique GPS L1 C/A PRNs 1 through 32")
            prns.add(prn)
            if "health" not in names and type(sv.get("healthy")) is not bool:
                raise ValueError("supply simulated SV health or a raw health column")
            indices, positions = [], []
            seen = set()
            for row in rows(sv["file"]):
                i = index(row)
                if i is None:
                    continue
                if i in seen:
                    raise ValueError(f"duplicate signal epoch for PRN {prn}")
                seen.add(i)
                healthy = (
                    int(row[names["health"]]) == 0
                    if "health" in names
                    else sv["healthy"]
                )
                if "cn0_dbhz" in names:
                    level = float(row[names["cn0_dbhz"]])
                else:
                    level = float(sv["reference_cn0_dbhz"]) + float(
                        row[names["relative_db"]]
                    )
                if not math.isfinite(level):
                    raise ValueError("non-finite signal level")
                if healthy and level >= Q["min_cno_dbhz"]:
                    indices.append(i)
                    positions.append([float(row[names[k]]) for k in XYZ[:3]])
            if not indices:
                continue
            i = np.asarray(indices)
            los = np.asarray(positions) - rx[i, :3]
            length = np.linalg.norm(los, axis=1)
            if not np.isfinite(length).all() or np.any(length <= 0):
                raise ValueError("invalid transmitted satellite position")
            los /= length[:, None]
            up = np.einsum("ij,ij->i", los, zenith[i]) >= sine_mask
            i = i[up]
            design = np.column_stack((los[up], np.ones(len(i))))
            normals[i] += np.einsum("ij,ik->ijk", design, design)
            visible[i] += 1
        pdop = np.full(count, 999.0)
        good = (visible >= 4) & (np.linalg.det(normals) > 1e-10)
        pdop[good] = np.sqrt(
            np.trace(np.linalg.inv(normals[good])[:, :3, :3], axis1=1, axis2=2)
        )
    return np.column_stack((np.arange(count) / hz, rx, visible, pdop, weeks, times))


def convert(archive_path, output, case):
    measured = measurements(archive_path, case)
    with output.open("x", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow([*TRUTH_HEADER, "gps_week", "tow_s"])
        for row in measured:
            writer.writerow([*row[:7], int(row[7]), row[8], int(row[9]), row[10]])
