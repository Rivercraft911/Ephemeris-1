"""Check the optional multi-SV RF mixer against the independent NumPy reference."""

from pathlib import Path
import sys
from tempfile import TemporaryDirectory

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from iqio import IF_HZ, RATE
from sky_signal import C, L1, NativeMixer, generate, path_bias_seconds
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


if __name__ == "__main__":
    # At zero angle, 1 TECU adds 16.24 cm at L1 to the code and advances carrier phase.
    code, carrier = path_bias_seconds(0, 0, 1, 0.75)
    np.testing.assert_allclose(
        [code * C, carrier * C], [0.75 + 40.3e16 / L1**2, 0.75 - 40.3e16 / L1**2]
    )
    mixer = NativeMixer(sys.argv[1])
    reference(mixer)
    seeded_rf(mixer)
