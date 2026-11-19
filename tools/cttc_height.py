"""Measure vertical geometry and code scatter in the CTTC rooftop replay.

Run without arguments to score the archived observables. To rebuild them, pass
--extract COMPACT.eiq GNSS-SDR.26O --output FILE.csv.gz. The receiver is built
from the measured revision and the reference RINEX contains C1C and L1C.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import csv
import gzip
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

import numpy as np

from compare_ground import match, position, reference_tracks, rows
from ground_fix import geodetic

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "docs/software/measurements/cttc"
OBSERVATIONS = DATA / "common-observations.csv.gz"
REVISION = "b546de5a679a8ab6ae554e431d01f4b714b7066d"
C = 299792458.
L1 = 1575420000.
FIELDS = ("raw_rx_tow_s", "gps_tow_s", "prn", "sat_x_m", "sat_y_m", "sat_z_m",
          "native_code_m", "rinex_code_m", "rinex_phase_cycles")


def trace_times(source, eiq):
    """Observe satellite positions and code times before the native position filter."""
    archive = subprocess.check_output(["git", "-C", ROOT, "archive", REVISION,
                                       "CMakeLists.txt", "receiver", "tools", "tests"])
    with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
        tree.extractall(source, filter="data")
    nav = source / "receiver/navigation.cpp"
    text = nav.read_text()
    marker = "    fix = trial;"
    if text.count(marker) != 1:
        raise ValueError("unexpected receiver source")
    nav.write_text("#include <cstdio>\n" + text.replace(marker, r"""
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = measurements[i];
        const auto xyz = reception_axes(m.sat.position_m, (m.range-trial.clock_bias_m)/speed_of_light);
        std::fprintf(stderr,"OBS,%.17g,%u,%.17g,%.17g,%.17g,%.17g\n",
                     trial.tow_s, obs[i].prn, obs[i].sv_tow_s, xyz[0], xyz[1], xyz[2]);
    }
""" + marker))
    build = source / "build"
    options = ["COARSE_TRACKING", "GROUND_SEARCH", "GROUND_TROPOSPHERE", "DOPPLER_POSITION", "VELOCITY_FIT"]
    subprocess.run(["cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
                    *["-DEPHEMERIS_" + opt + "=ON" for opt in options]], check=True)
    subprocess.run(["cmake", "--build", build, "--target", "eph-replay", "-j4"], check=True)
    result = subprocess.run([build / "eph-replay", eiq, "--work", "1141/5706"],
                            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    observations = np.array([[float(x) for x in line.split(",")[1:]]
                             for line in result.stderr.splitlines() if line.startswith("OBS,")])
    if not len(observations):
        raise ValueError("native replay produced no fixes")
    return {int(prn): observations[observations[:, 1] == prn]
            for prn in np.unique(observations[:, 1])}


def rinex_epochs(path):
    """Read the GNSS-SDR RINEX 3 GPS file produced by the CTTC reference config."""
    codes = []
    epoch = None
    values = {}
    for line in path.read_text().splitlines():
        if "SYS / # / OBS TYPES" in line and line.startswith("G"):
            codes = line[:60].split()[2:]
        if line.startswith(">"):
            if epoch is not None:
                yield epoch, values
            _, year, month, day, hour, minute, second, *_ = line.split()
            if (int(year), int(month), int(day)) != (2013, 4, 4):
                raise ValueError("expected the CTTC GPS date")
            epoch = 4 * 86400 + int(hour) * 3600 + int(minute) * 60 + float(second)
            values = {}
        elif epoch is not None and line.startswith("G"):
            def field(name):
                start = 3 + 16 * codes.index(name)
                return float(line[start:start + 14])
            values[int(line[1:3])] = (field("C1C"), field("L1C"))
    if epoch is not None:
        yield epoch, values


def extract(eiq, rinex, output):
    ref = {round(float(r["raw_rx_tow_s"]), 5): float(r["tow_s"])
           for r in rows(DATA / "gnss-sdr.csv.gz")}
    with tempfile.TemporaryDirectory() as tmp:
        traces = trace_times(Path(tmp), eiq)
    content = io.StringIO(newline="")
    writer = csv.writer(content, lineterminator="\n")
    writer.writerow(FIELDS)
    for raw, measurements in rinex_epochs(rinex):
        t = ref.get(raw)
        if t is None or len(measurements) != 5:
            continue
        if not all(a[0, 0] <= t <= a[-1, 0] for a in traces.values()):
            continue
        for prn, a in sorted(traces.items()):
            right = np.searchsorted(a[:, 0], t, side="right")
            right = min(right, len(a)-1)
            if a[right, 0] - a[max(0, right-1), 0] > 0.201:
                raise ValueError("missing native epoch in interpolation interval")
            tx, x, y, z = [np.interp(t, a[:, 0], col) for col in a[:, 2:].T]
            code, phase = measurements.get(prn, ("", ""))
            writer.writerow([raw, t, prn, f"{x:.4f}", f"{y:.4f}", f"{z:.4f}",
                             f"{C*(raw-tx):.5f}", code, phase])
    if content.getvalue().count("\n") < 2:
        raise ValueError("no common observation epochs")
    output.write_bytes(gzip.compress(content.getvalue().encode(), compresslevel=9, mtime=0))


def projection(group, xyz):
    satellites = np.array([[float(r[key]) for key in ("sat_x_m", "sat_y_m", "sat_z_m")] for r in group])
    rays = xyz - satellites
    rays /= np.linalg.norm(rays, axis=1)[:, None]
    design = np.column_stack((rays, np.ones(len(rays))))
    matrix = np.linalg.pinv(design)[:3]
    lat, lon = np.radians(geodetic(*xyz)[:2])
    up = np.array([np.cos(lat)*np.cos(lon), np.cos(lat)*np.sin(lon), np.sin(lat)])
    gains = up @ matrix
    vdop = np.linalg.norm(gains)
    return gains, vdop, np.sqrt(np.sum(matrix**2)-vdop**2)


def score(paired):
    pico = rows(DATA / "pico-aligned.csv.gz")
    track = reference_tracks(rows(DATA / "gnss-sdr.csv.gz"))[710]
    valid = [r for r in pico if r["valid"] == "1"]
    heights = []
    for r in valid:
        ref = match(track, float(r["tow_s"]))
        if ref is not None:
            heights.append((geodetic(*position(r))[2], geodetic(*ref)[2]))
    pico_h, ref_h = np.array(heights).T
    groups = {}
    for row in rows(paired):
        groups.setdefault(float(row["gps_tow_s"]), []).append(row)
    geometry = {}
    weights, native, reference, carrier = [], [], [], []
    common_ids = None
    for t, group in sorted(groups.items()):
        nearest = min(valid, key=lambda r: abs(float(r["tow_s"])-t))
        if abs(float(nearest["tow_s"])-t) > 0.11:
            raise ValueError("missing nearby Pico fix")
        xyz = position(nearest)
        active = int(nearest["nav_mask"])
        used = [r for r in group if active & (1 << (int(r["prn"])-1))]
        if len(used) != active.bit_count():
            raise ValueError("missing Pico satellite geometry")
        _, vdop, hdop = projection(used, xyz)
        geometry.setdefault(len(used), []).append((vdop, hdop))
        common = sorted((r for r in group if r["rinex_code_m"]), key=lambda r: int(r["prn"]))
        ids = [int(r["prn"]) for r in common]
        if common_ids is not None and ids != common_ids:
            raise ValueError("common satellites changed over the carrier comparison")
        common_ids = ids
        weights.append(projection(common, xyz)[0])
        native.append([float(r["native_code_m"]) for r in common])
        reference.append([float(r["rinex_code_m"]) for r in common])
        carrier.append([float(r["rinex_phase_cycles"])*C/L1 for r in common])
    weights, native, reference, carrier = map(np.array, (weights, native, reference, carrier))

    def clock_removed(measurements):
        return measurements - np.mean(measurements, axis=1)[:, None]

    code_up = np.sum(weights*clock_removed(native-reference), axis=1)
    mid = len(code_up)//2
    carrier_results = {}
    for label, codes in (("ephemeris_native", native), ("gnss_sdr", reference)):
        cmc = clock_removed(codes-carrier)
        # L1 phase has an unknown ambiguity per satellite. Set it only on the first half.
        vertical = np.sum(weights[mid:]*(cmc[mid:]-np.mean(cmc[:mid], axis=0)), axis=1)
        carrier_results[label] = {
            "pooled_code_sd_m": float(np.sqrt(np.var(cmc, axis=0).mean())),
            "held_out_p95_abs_vertical_change_m": float(np.percentile(abs(vertical), 95)),
        }
    delta = pico_h - ref_h
    return {
        "matched_physical_fixes": len(heights),
        "pico_height_sd_m": float(pico_h.std()),
        "reference_height_sd_m": float(ref_h.std()),
        "median_signed_up_m": float(np.median(delta)),
        "p95_abs_up_m": float(np.percentile(abs(delta), 95)),
        "common_prns": common_ids,
        "common_epochs": len(code_up),
        "held_out_epochs": len(code_up)-mid,
        "pico_geometry": {n: dict(zip(("median_vdop", "median_hdop"), np.median(g, axis=0)))
                          for n, g in geometry.items()},
        "common_code_p95_abs_up_m": float(np.percentile(abs(code_up), 95)),
        "common_code_median_signed_up_m": float(np.median(code_up)),
        "carrier": carrier_results,
    }


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--extract", nargs=2, type=Path, metavar=("COMPACT_EIQ", "GNSS_SDR_RINEX"))
    ap.add_argument("--output", type=Path, help="destination for extracted observables")
    args = ap.parse_args()
    if bool(args.extract) != bool(args.output):
        ap.error("--extract and --output must be used together")
    if args.extract:
        extract(*args.extract, args.output)
    print(json.dumps(score(args.output or OBSERVATIONS), indent=2))
