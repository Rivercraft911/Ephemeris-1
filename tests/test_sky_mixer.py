"""Check the optional multi-SV RF mixer against the independent NumPy reference."""

from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from unittest.mock import Mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from ground_propagation import summary
from iqio import IF_HZ, RATE
from sky_signal import (
    C, L1, START, NativeMixer, generate, geometry, ground_delays, path_bias_seconds,
    receiver, visibility,
)
from vectors import chips


def reference(mixer):
    # At the end of an orbit the absolute C/A coordinate exceeds 32 bits.
    bit = (2**32 // 20460) + 1
    rng = np.random.default_rng(17)
    code = chips(15)
    nav = rng.choice(np.array([-1, 1], np.int8), size=bit + 3)
    nav[bit : bit + 2] = [1, -1]
    table = np.exp(2j * np.pi * np.arange(2048) / 2048).astype(np.complex64)
    start = bit * 20460 + 20458.125
    for shift, scale in [(42000.0, 1.000002), (-48500.0, 0.999998)]:
        local = np.arange(50000) / RATE / scale
        hz = 1023000 + shift / 1540
        chip = np.floor(start + hz * local).astype(np.int64)
        phase = np.floor(2048 * (0.77 + (IF_HZ + shift) * local)).astype(np.int64)
        amplitude = np.float64(0.2125)
        noise = rng.standard_normal(
            len(local), dtype=np.float32
        ) + 1j * rng.standard_normal(len(local), dtype=np.float32)
        expected = noise.copy()
        expected += (
            amplitude * code[chip % 1023] * nav[chip // 20460] * table[phase & 2047]
        )
        mixer.add(
            noise, local, start, hz, 0.77, IF_HZ + shift, amplitude, code, nav, table
        )
        np.testing.assert_array_equal(noise, expected)


def seeded_rf(mixer):
    with TemporaryDirectory() as tmp:
        base = Path(tmp)
        for name, source in [("numpy", None), ("native", mixer)]:
            generate(
                base / (name + ".eiq"),
                base / (name + ".csv"),
                0.21,
                6,
                550,
                997,
                43,
                "leo-550-3",
                "frequency-high",
                source,
            )
        for suffix in (".csv", ".eiq"):
            assert (base / ("numpy" + suffix)).read_bytes() == (
                base / ("native" + suffix)
            ).read_bytes()


def ground_rf(mixer):
    # Mirror the receiving antenna in a plane 8 m below it and use Euclidean
    # lengths as an independent oracle for the far-field excess path.
    sine = np.array([0.5, 1.0])
    neutral, extra = ground_delays(sine, 2.4, 8)
    np.testing.assert_allclose(neutral * C, [4.7857, 2.4], atol=5e-5)
    dx = 20_000_000 * np.sqrt(1 - sine**2)
    dz = 20_000_000 * sine
    np.testing.assert_allclose(extra * C, np.hypot(dx, dz + 16) - 20_000_000, atol=1e-5)
    with TemporaryDirectory() as tmp:
        base = Path(tmp)
        for name, source in [("numpy", None), ("native", mixer)]:
            generate(
                base / f"{name}.eiq", base / f"{name}.csv", 0.020, 7, 0, 1958, 47,
                mixer=source, zenith_delay_m=2.4, ground_echo=(8, -0.3),
            )
        assert (base / "numpy.eiq").read_bytes() == (base / "native.eiq").read_bytes()

        # Inspect the first satellite's two paths before the quantizer. Delay
        # must appear in the chips, carrier phase and both rates.
        tap = Mock()
        generate(
            base / "tap.eiq", base / "tap.csv", 0.010, 7, 0, 1958, 47,
            mixer=tap, ground_echo=(8, -0.3),
        )
        direct, echo = [c.args for c in tap.add.call_args_list[:2]]
        sats = geometry(1958, 7, 0)
        rx = receiver(0, 0)[0]
        delta = np.array([16 * visibility(sats, START + t, rx)[1][0] / C for t in (0, 0.01)])
        rate = (delta[1] - delta[0]) / 0.01
        np.testing.assert_allclose(direct[2] - echo[2], 1.023e6 * delta[0], atol=5e-9)
        np.testing.assert_allclose(direct[3] - echo[3], 1.023e6 * rate, atol=2e-10)
        np.testing.assert_allclose((direct[4] - echo[4]) % 1, (L1 * delta[0]) % 1)
        np.testing.assert_allclose(direct[5] - echo[5], L1 * rate, atol=5e-10)
        np.testing.assert_allclose(echo[6] / direct[6], -0.3)
        assert direct[7] is echo[7] and direct[8] is echo[8]

        with np.testing.assert_raises_regex(ValueError, "static ground receiver"):
            generate(
                base / "leo.eiq", base / "leo.csv", 0.01, 6, 400, 1, 47,
                zenith_delay_m=2.4,
            )


def ground_scores():
    truth = [
        dict(t_s=i / 5, x_m=6378137, y_m=0, z_m=0, vx_mps=0, vy_mps=0,
             vz_mps=0, gps_week=2351, tow_s=345600 + i / 5)
        for i in range(7)
    ]
    rows = [
        dict(ref, sample=round(ref["t_s"] * RATE), x_m=6378148, week_mod1024=303,
             valid="1", gaps="0", locked_mask="127", nav_mask="127", sv="7")
        for ref in truth[1:]
    ]
    scores = summary(truth, rows)
    assert scores["first_valid_s"] == 0.2 and scores["valid_epochs"] == 6
    assert scores["first_five_within_10m_0p2mps_s"] is None
    assert scores["mean_up_error_m"] == scores["position_p95_m"] == 11
    assert scores["horizontal_p95_m"] == 0
    with np.testing.assert_raises_regex(ValueError, "5 Hz"):
        summary(truth, rows[1:])


if __name__ == "__main__":
    # At zero angle, 1 TECU adds 16.24 cm at L1 to the code and advances carrier phase.
    code, carrier = path_bias_seconds(0, 0, 1, 0.75)
    np.testing.assert_allclose(
        [code * C, carrier * C], [0.75 + 40.3e16 / L1**2, 0.75 - 40.3e16 / L1**2]
    )
    mixer = NativeMixer(sys.argv[1])
    reference(mixer)
    seeded_rf(mixer)
    ground_rf(mixer)
    ground_scores()
