"""Attribute position errors in the 165 s orbital replays. SPDX-License-Identifier: Apache-2.0"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

import numpy as np

RECEIVER = "6938dc5753578d0b3aaf1741253bf5423e03d54e"
GENERATOR = "28ca29a6719475195e3aabd5930c4ed02d67190f"
C = 299792458.0
FS = 16368000
START_TOW_S = 525609.0
SECONDS = 165
STATUS_HZ = 5
SIM_HASHES = {
    "gpssim.c": "e1707b112d9f5291b06b95ab5af17e1b83c9399e35ccb968467db4f282437b24",
    "gpssim.h": "093c341c6045bbfacc70df20ef7dc0f1a63e88741606808b44fe7463f2862fe4",
    "brdc0010.22n": "7db04513dd2d0e13c0ee20cb4eaa8f71e5a28ab58b65c9b5b789f86eeab436cd",
}


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def insert(path, marker, addition):
    source = path.read_text()
    if source.count(marker) != 1:
        raise ValueError(f"unknown instrument location in {path.name}")
    path.write_text(source.replace(marker, addition + marker))


def trace_build(root, simulator, temp):
    # Keep the receiver and generator working trees untouched. Observe only valid solves.
    archive = subprocess.check_output(["git", "-C", root, "archive", RECEIVER, "CMakeLists.txt", "receiver", "tests", "tools"])
    src = temp / "source"
    with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
        tree.extractall(src, filter="data")
    sys.path[:0] = [str(src / "tests"), str(src / "tools")]
    nav = src / "receiver/navigation.cpp"
    insert(nav, '#include "navigation.hpp"', "#include <cstdio>\n#include <cstdlib>\n")
    insert(nav, "    fix = trial;", '''
    static auto* trace = std::fopen(std::getenv("RANGE_TRACE"), "w");
    if (!trace) std::abort();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = measurements[i];
        const double travel = (m.range - trial.clock_bias_m) / speed_of_light;
        const auto s = reception_axes(m.sat.position_m, travel);
        std::fprintf(trace,"%u,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g\\n",
                     obs[i].prn, trial.tow_s, obs[i].sv_tow_s, m.range, s[0], s[1], s[2],
                     trial.position_m[0], trial.position_m[1], trial.position_m[2], trial.pdop);
    }
''')
    build = temp / "build"
    flags = ["-DCMAKE_BUILD_TYPE=Release", "-DEPHEMERIS_COARSE_TRACKING=ON",
             "-DEPHEMERIS_FLIGHT_SURVEY=ON", "-DEPHEMERIS_VELOCITY_FIT=ON"]
    subprocess.run(["cmake", "-S", src, "-B", build, *flags], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", build, "--target", "eph-replay", "-j4"], check=True, stdout=subprocess.DEVNULL)
    upstream = temp / "gpssim_trace.c"
    upstream.write_bytes((simulator / "gpssim.c").read_bytes())
    insert(upstream, "computeCodePhase(&chan[i], rho, 0.1);",
           'printf("%d,%d,%.17g\\n",iumd,chan[i].prn,rho.range);\n')
    # computeRange and channel allocation precede sample synthesis. Skip only the I/Q output.
    insert(upstream, "for (iumd=1; iumd<numd; iumd++)", "iq_buff_size = 0;\n")
    oracle = temp / "range-oracle"
    subprocess.run(["cc", "-O3", "-I", simulator, upstream, "-lm", "-o", oracle], check=True)
    return build / "eph-replay", oracle


def score(trace, ranges, status, altitude, phase):
    from campaign import OMEGA
    from independent_orbit import trajectory
    oracle = {(int(i), int(p)): r for i, p, r in ranges}
    expected_samples = np.arange(1, SECONDS * STATUS_HZ + 1) * (FS // STATUS_HZ)
    if not np.array_equal(status["sample"], expected_samples):
        raise ValueError("expected every 5 Hz status report from the 165-second recording")
    fixes = status[status["valid"] == 1]
    rows = []
    totals = []
    for fix in fixes:
        sample = int(fix["sample"])
        t = sample / FS
        gt = trajectory(np.array([t]), altitude, phase)[0]
        # CSV gives fix time to nine decimal places. Use the sample index for truth.
        sv = trace[np.abs(trace[:, 1] - fix["tow_s"]) < 1e-6]
        prns = {int(prn) for prn in sv[:, 0]}
        if (len(prns) != len(sv) or len(prns) != fix["sv"]
                or sum(1 << (prn - 1) for prn in prns) != int(fix["nav_mask"])):
            raise ValueError("mismatched observation set")
        line = gt[:3] - sv[:, 4:7]
        rho = np.linalg.norm(line, axis=1)
        project = np.linalg.pinv(np.column_stack((line / rho[:, None], np.ones(len(sv)))))[:3]
        source = np.array([oracle[round(t * 10), int(prn)] for prn in sv[:, 0]])
        measured = sv[:, 3] - rho
        timing = C * ((START_TOW_S - sv[:, 2]) + t) - source
        measured -= measured.mean()
        timing -= timing.mean()
        model = measured - timing
        delta = sv[0, 7:10] - gt[:3]
        radial = gt[:3] / np.linalg.norm(gt[:3])
        normal = np.cross(radial, gt[3:] + np.cross([0., 0., OMEGA], gt[:3]))
        normal /= np.linalg.norm(normal)
        rtn = np.array([radial, np.cross(normal, radial), normal]) @ delta
        total, received, predicted = (np.linalg.norm(v) for v in (delta, project @ timing, project @ model))
        rows.append([len(sv), sv[0, 10], total, received, predicted, np.linalg.norm(project @ measured - delta), *rtn])
        totals.extend(np.column_stack((measured, timing, model)))
    rows = np.array(rows)
    def stats(data):
        vals = np.percentile(data[:, 1:5], 95, axis=0)
        return dict(fixes=len(data), median_pdop=round(float(np.median(data[:, 1])), 4),
                    p95_pdop=round(float(vals[0]), 4), p95_position_m=round(float(vals[1]), 4),
                    p95_from_code_timing_m=round(float(vals[2]), 4), p95_from_range_model_m=round(float(vals[3]), 4),
                    radial_along_cross_rms_m=np.sqrt(np.mean(data[:, 6:]**2, axis=0)).round(4).tolist())
    rms = np.sqrt(np.mean(np.array(totals)**2, axis=0)).round(5)
    return dict(full=stats(rows), by_satellite_count={int(n): stats(rows[rows[:, 0] == n]) for n in np.unique(rows[:, 0])},
                pooled_clock_removed_range_rms_m=dict(zip(("all", "code_timing", "range_model"), rms)),
                max_linear_reconstruction_error_m=float(rows[:, 5].max()))


def run(root, simulator, cases, temp):
    for name, checksum in SIM_HASHES.items():
        if digest(simulator / name) != checksum:
            raise ValueError(f"unexpected simulator source {name}")
    replay, oracle = trace_build(root, simulator, temp)
    # The upstream CLI accepts short filenames. Run it inside our scratch directory.
    nav_file = "brdc0010.22n"
    shutil.copyfile(simulator / nav_file, temp / nav_file)
    from independent_orbit import trajectory
    archive = json.loads(subprocess.check_output(["git", "-C", root, "show", f"{RECEIVER}:docs/software/measurements/flight-survey.json"]))
    result = dict(receiver_revision=RECEIVER, gps_sdr_sim_revision=GENERATOR, upstream_sha256=SIM_HASHES,
                  units="Meters for range and position. Median and p95 PDOP are dimensionless.",
                  method="Clock-removed receive code times versus the waveform generator's scheduled L1 C/A ranges. The remaining differential range is the simulator/receiver range-model difference. Position contributions use the actual unweighted least-squares line of sight and clock column for each successful fix. Double-precision receiver positions are used before CSV rounding. Component percentiles are separate distributions.",
                  orbits=[])
    published = {(row["altitude_km"], row["phase_deg"], row["noise_seed"]): row for row in archive["orbits"]}
    for km, arglat, noise_seed, source in cases:
        altitude, phase, seed = int(km), float(arglat), int(noise_seed)
        checksum = digest(Path(source))
        case = published.get((altitude, phase, seed))
        if case and checksum != case["input_sha256"]:
            raise ValueError(f"wrong I/Q recording for {altitude} km")
        t = np.arange(10 * SECONDS + 1) / 10
        motion = temp / "motion.csv"
        np.savetxt(motion, np.column_stack((t, trajectory(t, altitude, phase)[:, :3])), fmt="%.9f", delimiter=",")
        trace, status, ranges = [temp / name for name in ("observations.csv", "status.csv", "oracle.csv")]
        trace.unlink(missing_ok=True)
        with status.open("w") as out:
            subprocess.run([replay, source, "--work", "1141/5706"], stdout=out, check=True, env=dict(os.environ, RANGE_TRACE=str(trace)))
        if case and digest(status) != case["survey"]["status_sha256"]:
            raise ValueError(f"receiver output differs from archived {altitude} km replay")
        with ranges.open("w") as out:
            subprocess.run([oracle, "-e", nav_file, "-u", motion.name, "-t", "2022/01/01,02:00:09",
                            "-s", "16368000", "-b", "8", "-i", "-d", "165.1", "-o", os.devnull], check=True,
                           cwd=temp, stdout=out, stderr=subprocess.DEVNULL)
        if not trace.exists():
            raise ValueError(f"no valid fixes to attribute for the {altitude} km case at phase {phase}")
        data = score(np.loadtxt(trace, delimiter=","), np.loadtxt(ranges, delimiter=","),
                     np.genfromtxt(status, names=True, delimiter=","), altitude, phase)
        result["orbits"].append(dict(altitude_km=altitude, phase_deg=phase, noise_seed=seed,
                                     input_sha256=checksum, status_sha256=digest(status), **data))
    return result


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("simulator", type=Path, help="pinned GPS-SDR-SIM source checkout")
    p.add_argument("--case", nargs=4, action="append", required=True, metavar=("KM", "PHASE_DEG", "SEED", "EIQ"),
                   help="165 s January 2022 orbital fixture; repeat for additional orbits")
    args = p.parse_args()
    root = Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip())
    with tempfile.TemporaryDirectory() as scratch:
        print(json.dumps(run(root, args.simulator.resolve(), args.case, Path(scratch)), indent=2, allow_nan=False))
