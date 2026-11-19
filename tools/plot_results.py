"""Plot native cold starts and Pico processing time from the recorded results.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

BASE = Path(__file__).resolve().parents[1] / "docs/software/measurements"
PALETTE = ["#2fa8ff", "#ff8c3a", "#6fdc5c"]


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
    for col, (label, color, line) in enumerate(
        (
            ("Visible in simulated sky", "#dedede", ":"),
            ("Tracking lock", PALETTE[0], "--"),
            ("Usable navigation message", PALETTE[1], "-."),
            ("Used in valid fix", PALETTE[2], "-"),
        ),
        1,
    ):
        ax.step(
            changes[:, 0],
            changes[:, col],
            where="post",
            color=color,
            ls=line,
            lw=1.5 if col < 4 else 2.2,
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
        title="RP2350B live tracking",
        xticks=x,
        xticklabels=[r["channels"] for r in profiles],
        ylim=(0, max(values) * 1.17),
    )
    ax.grid(axis="y", linewidth=0.45, alpha=0.55)
    ax.legend(frameon=False, loc="upper left")
    fig.savefig(destination / "pico-timing.png", dpi=180)
    plt.close(fig)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--results", type=Path, default=BASE / "baseline.json")
    p.add_argument("--out", type=Path, default=BASE)
    a = p.parse_args()
    make_figures(json.loads(a.results.read_text()), a.out)
