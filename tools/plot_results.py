"""Plot native cold starts and Pico processing time from the recorded results.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

BASE = Path(__file__).resolve().parents[1] / "docs/software/measurements"
PALETTE = ["#2fa8ff", "#ff8c3a", "#6fdc5c"]


def collect_orbits(directory):
    """Minute-by-minute errors for the three nominal 5 Hz, zero-offset software replays."""
    from campaign import FS, Q, XYZ, orbit_period, table
    from run_campaign import digest

    result = []
    for km in (400, 550, 800):
        run = directory / f"leo-{km}-1" / "nominal"
        report = json.loads((run / "report.json").read_text())
        if (
            report["level"] != "native raw I/Q replay"
            or report["stress"] != "nominal"
            or report["scenario"] != f"leo-{km}-1"
            or report["seconds"] < math.ceil(orbit_period(km))
        ):
            raise ValueError(f"expected a complete nominal orbital replay in {run}")
        for name, key in (("truth.csv", "truth_sha256"), ("receiver.csv", "output_sha256")):
            if digest(run / name) != report[key]:
                raise ValueError(f"{run / name} differs from the scored recording")
        truth = table(run / "truth.csv")
        output = table(run / "receiver.csv")
        sample_truth = {round(float(r["t_s"]) * FS): r for r in truth}
        first_fix = report["result"]["ttff_s"]
        first_sample = round(first_fix * FS) if first_fix is not None else math.inf
        windows = {}
        for row in output:
            sample = int(row["sample"])
            if row["valid"] != "1" or sample < first_sample:
                continue
            ref = sample_truth[sample]
            if (
                float(ref["healthy_sv"]) < Q["min_healthy_sv"]
                or float(ref["pdop"]) > Q["max_pdop"]
            ):
                continue
            diff = np.array([float(row[k]) - float(ref[k]) for k in XYZ])
            windows.setdefault(int(sample / FS / 60), []).append(
                [np.linalg.norm(diff[:3]), np.linalg.norm(diff[3:])]
            )
        points = [
            [minute, *np.percentile(group, 95, axis=0).round(5)]
            for minute, group in sorted(windows.items())
        ]
        result.append(
            {
                "km": km,
                "seconds": report["seconds"],
                "result": report["result"],
                "truth_sha256": report["truth_sha256"],
                "receiver_csv_sha256": report["output_sha256"],
                "receiver_sha256": report["receiver_sha256"],
                "suite_sha256": report["suite_sha256"],
                "generator_sources_sha256": report["generator_sources_sha256"],
                "mixer_sha256": report["mixer_sha256"],
                "noise_seed": int(report["generator"][report["generator"].index("--seed") + 1]),
                "work_steps_per_ms": int(report["decoder"][report["decoder"].index("--work") + 1]),
                "minute_p95": points,
            }
        )
    return result


def make_figures(data, destination):
    plt.rcParams.update(
        {
            "figure.facecolor": "#1f1f1f",
            "axes.facecolor": "#141414",
            "text.color": "#dedede",
            "axes.labelcolor": "#dedede",
            "axes.edgecolor": "#999999",
            "xtick.color": "#dedede",
            "ytick.color": "#dedede",
            "grid.color": "#777777",
            "font.family": "sans-serif",
            "font.size": 9,
            "axes.titlesize": 10,
            "axes.linewidth": 0.65,
            "xtick.direction": "in",
            "ytick.direction": "in",
            "xtick.top": True,
            "ytick.right": True,
            "axes.axisbelow": True,
        }
    )
    destination.mkdir(parents=True, exist_ok=True)
    fig, axes = plt.subplots(1, 3, figsize=(9, 3.2), layout="constrained")
    metrics = (
        ("ttff_s", "First fix (s)"),
        ("p95_position_m", "3D position error, p95 (m)"),
        ("p95_velocity_mps", "3D velocity error, p95 (m/s)"),
    )
    for ax, (key, title) in zip(axes, metrics):
        for i, km in enumerate((400, 550, 800)):
            rows = sorted(
                (r for r in data["cold"] if r["km"] == km), key=lambda r: r["start"]
            )
            # A small horizontal offset exposes identical values at different altitudes.
            ax.scatter(
                [r["start"] + 0.12 * (i - 1) for r in rows],
                [r[key] for r in rows],
                s=34,
                marker=("o", "s", "^")[i],
                color=PALETTE[i],
                label=f"{km} km",
            )
        ax.set(
            title=title,
            xlabel="Cold start",
            xticks=[1, 2, 3, 4],
            xlim=(0.6, 4.4),
            ylim=(0, None),
        )
        ax.grid(axis="y", linewidth=0.45, alpha=0.55)
    axes[0].legend(loc="lower left", fontsize=8, frameon=False)
    fig.savefig(destination / "cold-starts.png", dpi=180)
    plt.close(fig)
    fig, ax = plt.subplots(figsize=(6.6, 3.15), layout="constrained")
    example = data["example"]
    changes = np.array(example["changes"])
    changes = np.vstack((changes, np.r_[example["seconds"], changes[-1, 1:]]))
    ax.fill_between(
        changes[:, 0],
        0,
        changes[:, 1],
        step="post",
        color="#cccccc",
        alpha=0.18,
        label="Visible in simulated sky",
    )
    series = [(2, "Tracking lock", PALETTE[0], "--")]
    if np.array_equal(changes[:, 3], changes[:, 4]):
        series.append((4, "Decoded and used in fix", PALETTE[2], "-"))
    else:
        series.extend(
            [
                (3, "Usable navigation message", PALETTE[1], "-."),
                (4, "Used in valid fix", PALETTE[2], "-"),
            ]
        )
    for col, label, color, line in series:
        ax.step(
            changes[:, 0],
            changes[:, col],
            where="post",
            color=color,
            ls=line,
            lw=1.5,
            label=label,
        )
    ax.set(
        xlabel="Time from cold start (s)",
        ylabel="GPS satellites",
        title="From antenna visibility to first fix, 550 km",
        xlim=(0, 75),
        ylim=(-0.3, 8.5),
        yticks=(0, 2, 4, 6, 8),
    )
    ax.legend(frameon=False, loc="lower right", fontsize=8)
    ax.grid(axis="y", linewidth=0.45, alpha=0.55)
    fig.savefig(destination / "satellite-lock.png", dpi=180)
    plt.close(fig)
    fig, ax = plt.subplots(figsize=(6.6, 3.15), layout="constrained")
    profiles = data["pico"]["live"]
    x = np.arange(len(profiles))
    values = [r["max_us"] / 1000 for r in profiles]
    ax.bar(x, values, width=0.44, color=PALETTE[0])
    ax.axhline(1, color=PALETTE[1], lw=1.4, ls="--", label="1 ms input deadline")
    for pos, v in zip(x, values):
        ax.annotate(
            f"{v:.2f}", (pos, v), xytext=(0, 4), textcoords="offset points", ha="center"
        )
    ax.set(
        xlabel="Seeded GPS channels",
        ylabel="Worst processing time (ms)",
        title="Scalar tracking on the 150 MHz RP2350B",
        xticks=x,
        xticklabels=[r["channels"] for r in profiles],
        ylim=(0, max(values) * 1.17),
    )
    ax.grid(axis="y", linewidth=0.45, alpha=0.55)
    ax.legend(frameon=False, loc="upper left")
    fig.savefig(destination / "pico-timing.png", dpi=180)
    plt.close(fig)

    if "coarse_dual_offline" in data["pico"]:
        fig, ax = plt.subplots(figsize=(6.6, 3.3), layout="constrained")
        for key, label, color, marker in (
            ("offline", "Scalar", "#dedede", "o"),
            ("prefix_offline", "Shared prefixes", PALETTE[1], "s"),
            ("coarse_serial_offline", "Half-chip, one core", PALETTE[0], "^"),
            ("coarse_dual_offline", "Half-chip, two cores", PALETTE[2], "D"),
        ):
            records = data["pico"][key]
            ax.plot(
                [r["channels"] for r in records],
                [r["steady_mean_us"] / 1000 for r in records],
                label=label,
                color=color,
                marker=marker,
                markersize=4,
            )
        ax.axhline(1, ls=":", color="#dedede", label="1 ms budget")
        ax.set(
            yscale="log",
            xticks=(1, 4, 6, 8),
            xlabel="Seeded GPS channels",
            ylabel="Mean processing time (ms)",
            title="Post-lock processing on the 150 MHz RP2350B",
        )
        ax.grid(axis="y", which="both", linewidth=0.4, alpha=0.5)
        ax.legend(frameon=False, fontsize=8, loc="upper left", ncol=2)
        fig.savefig(destination / "tracking-cost.png", dpi=180)
        plt.close(fig)
    if data.get("orbits"):
        fig, axes = plt.subplots(
            2, 1, figsize=(7.4, 4.4), sharex=True, layout="constrained"
        )
        for case, color, line in zip(data["orbits"], PALETTE, ("-", "--", "-.")):
            if not case["minute_p95"]:
                continue
            points = np.asarray(case["minute_p95"])
            minutes = np.arange(int(points[-1, 0]) + 1)
            errors = np.full((len(minutes), 2), np.nan)
            errors[points[:, 0].astype(int)] = points[:, 1:]
            for col, ax in enumerate(axes):
                ax.plot(
                    minutes + 0.5,
                    errors[:, col],
                    color=color,
                    ls=line,
                    label=f'{case["km"]} km',
                )
        axes[0].set(ylabel="3D position (m)", title="95th-percentile error by minute")
        axes[1].set(ylabel="3D velocity (m/s)", xlabel="Elapsed orbital time (min)")
        for ax in axes:
            ax.grid(axis="y", linewidth=0.45, alpha=0.55)
            ax.set_ylim(bottom=0)
        axes[0].legend(frameon=False, ncol=3)
        fig.savefig(destination / "orbital-error.png", dpi=180)
        plt.close(fig)


def ground_figure(data, destination):
    labels = {
        "clean": "No atmosphere or echo",
        "neutral": "Neutral delay, 2.4 m",
        "echo": "Reflection only",
        "combined": "Neutral delay + reflection",
        "heldout-clean": "No atmosphere or echo",
        "heldout-dry": "Neutral delay, 1.9 m",
        "heldout-wet-echo": "2.8 m + reflection",
    }
    fig, ax = plt.subplots(figsize=(7.2, 3.7), layout="constrained")
    rows = data["cases"]
    y = np.arange(len(rows))
    for i, (key, label) in enumerate((
        ("uncorrected", "Raw solution"),
        ("corrected", "Atmosphere correction"),
        ("corrected_with_doppler", "Correction + Doppler"),
    )):
        ax.scatter([r[key]["position_p95_m"] for r in rows], y + (i - 1) * 0.15,
                   color=PALETTE[i], marker=("o", "s", "^")[i], label=label, s=32)
    ax.axhline(3.5, color="#888888", lw=0.65)
    ax.set(yticks=y, yticklabels=[labels[r["name"]] for r in rows],
           xlabel="95th-percentile 3D position error (m)",
           title="Ground receiver, two simulated skies", xlim=(0, None))
    ax.invert_yaxis()
    ax.grid(axis="x", linewidth=0.45, alpha=0.55)
    ax.legend(frameon=False, loc="lower center", bbox_to_anchor=(0.5, -0.34), ncol=3, fontsize=8)
    fig.savefig(destination / "ground-accuracy.png", dpi=180)
    plt.close(fig)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--results", type=Path, default=BASE / "baseline.json")
    p.add_argument("--out", type=Path, default=BASE)
    p.add_argument(
        "--import-orbits",
        type=Path,
        help="refresh the orbit measurements in --results from completed nominal reports",
    )
    a = p.parse_args()
    data = json.loads(a.results.read_text())
    if a.import_orbits:
        data["orbits"] = collect_orbits(a.import_orbits)
        a.results.write_text(json.dumps(data, indent=2, allow_nan=False) + "\n")
    make_figures(data, a.out)
    ground = a.results.with_name("ground-bias.json")
    if ground.exists():
        ground_figure(json.loads(ground.read_text()), a.out)
