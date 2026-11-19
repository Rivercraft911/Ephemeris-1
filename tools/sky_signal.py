"""Generate a deterministic multi-SV GPS L1 C/A EIQ recording and separate truth.

Clean broadcast-only, synthetic circular GPS ephemerides and static or circular
receiver motion. Carrier, code phase and LNAV bits share the same retarded
transmit time. No velocity, orbit, time or PRN hint enters the EIQ header.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
from contextlib import nullcontext
import csv
import ctypes
import math
import sys
from pathlib import Path

import numpy as np

from campaign import SPEC, TRUTH_HEADER
from iqio import HEADER, IF_HZ, RATE, SAMPLES_MS
from vectors import chips, pack
from stress import Stress

C = 299792458.0
L1 = 1575420000.0
MU = 3.986005e14
OMEGA = 7.2921151467e-5
TOE = 345600
START = TOE - 5
WEEK = 2351
SQRT_A = round(math.sqrt(26_560_000) * 2**19)
# Parity equations expressed in ICD data-bit positions rather than receiver masks.
PARITY = (
    (1, 2, 3, 5, 6, 10, 11, 12, 13, 14, 17, 18, 20, 23),
    (2, 3, 4, 6, 7, 11, 12, 13, 14, 15, 18, 19, 21, 24),
    (1, 3, 4, 5, 7, 8, 12, 13, 14, 15, 16, 19, 20, 22),
    (2, 4, 5, 6, 8, 9, 13, 14, 15, 16, 17, 20, 21, 23),
    (1, 3, 5, 6, 7, 9, 10, 14, 15, 16, 17, 18, 21, 22, 24),
    (3, 5, 6, 8, 9, 10, 11, 13, 15, 19, 22, 23, 24),
)


def word(data, history):
    parity = 0
    for i, eq in enumerate(PARITY):
        carry = (history >> (1 if i in (0, 2, 5) else 0)) & 1
        v = (sum((data >> (24 - b)) & 1 for b in eq) + carry) % 2
        parity = parity * 2 + v
    wire = data ^ (0xFFFFFF if history & 1 else 0)
    return wire * 64 + parity


def angle_bits(rad):
    return round(math.remainder(rad, 2 * math.pi) / math.pi * 2**31) & 0xFFFFFFFF


def angle_rad(bits):
    return (bits if bits < 2**31 else bits - 2**32) * math.pi / 2**31


class SpaceVehicle:
    def __init__(self, prn, node, anomaly, toe=TOE):
        self.toe = toe
        self.prn = prn
        self.node = angle_bits(node)
        self.m0 = angle_bits(anomaly)
        self.inc = angle_bits(math.radians(55))
        self.clock_raw = 1800 * prn - 5500  # mixed signs, different SV clocks
        self.clock = self.clock_raw * 2**-31
        self.code = chips(prn)

    def position(self, dt):
        a = (SQRT_A * 2**-19) ** 2
        u = angle_rad(self.m0) + math.sqrt(MU / a**3) * dt
        node = angle_rad(self.node) - OMEGA * (self.toe + dt)
        inc = angle_rad(self.inc)
        x = a * math.cos(u)
        y = a * math.sin(u)
        return np.array(
            [
                x * math.cos(node) - y * math.cos(inc) * math.sin(node),
                x * math.sin(node) + y * math.cos(inc) * math.cos(node),
                y * math.sin(inc),
            ]
        )

    def nav(self, week, tow0, duration):
        sf1 = [
            (week % 1024) << 14,
            0,
            0,
            0,
            0,
            1 << 16 | self.toe // 16,
            0,
            (self.clock_raw & 0x3FFFFF) << 2,
        ]
        sf2 = [
            1 << 16,
            self.m0 >> 24,
            self.m0 & 0xFFFFFF,
            0,
            0,
            SQRT_A >> 24,
            SQRT_A & 0xFFFFFF,
            self.toe // 16 << 8,
        ]
        sf3 = [
            self.node >> 24,
            self.node & 0xFFFFFF,
            self.inc >> 24,
            self.inc & 0xFFFFFF,
            0,
            0,
            0,
            1 << 16,
        ]

        # SF4/5 carry almanac-format payloads. They supply normal bit
        # transitions while this L1-only receiver waits for the next SF1.
        def almanac(prn):
            return [
                (1 << 22) | (prn << 16),
                ((self.toe // 4096) & 255) << 16,
                0,
                SQRT_A >> 8,
                self.node >> 8,
                0,
                self.m0 >> 8,
                0,
            ]

        sf4, sf5 = almanac(25), almanac(1)
        packed = []
        for t in range(tow0, math.ceil(tow0 + duration) + 6, 6):
            sf = int(t // 6 % 5)
            data = [0x8B0000, ((t // 6 + 1) % 100800) << 7 | (sf + 1) << 2]
            data += (sf1, sf2, sf3, sf4, sf5)[sf]
            history = 0
            for j, d in enumerate(data):
                if j in (1, 9):
                    # D23/D24 are non-information-bearing; use them to carry
                    # a zero D29/D30 into the next TLM or hand-over boundary.
                    w = next(
                        v
                        for v in (word(d | i, history) for i in range(4))
                        if v & 3 == 0
                    )
                else:
                    w = word(d, history)
                history = w & 3
                packed.extend((w >> b) & 1 for b in range(29, -1, -1))
        return 1 - 2 * np.array(packed, dtype=np.int8)


def receiver(t, height, phase=0.0, node=0.0, incl=97.6):
    a = 6378137.0 + 1000 * height
    if not height:
        return np.array([a, 0.0, 0.0]), np.zeros(3)
    nu = math.sqrt(MU / a**3)
    ci, si = math.cos(math.radians(incl)), math.sin(math.radians(incl))
    u, lon = nu * t + math.radians(phase), math.radians(node) - OMEGA * t
    co, so = math.cos(lon), math.sin(lon)
    x, y = a * math.cos(u), a * math.sin(u)
    pos = np.array([co * x - so * ci * y, so * x + co * ci * y, si * y])
    xd, yd = -nu * y, nu * x
    vel = np.array(
        [
            co * xd - so * ci * yd + OMEGA * pos[1],
            so * xd + co * ci * yd - OMEGA * pos[0],
            si * yd,
        ]
    )
    return pos, vel


def clock_range(sv, tow, rx):
    """L1 code delay including receiver-epoch axes and broadcast SV clock."""
    tau = 0.075
    for _ in range(3):
        p = sv.position(tow - sv.toe - tau)
        theta = tau * OMEGA
        ct, st = math.cos(theta), math.sin(theta)
        delta = (
            ct * p[0] + st * p[1] - rx[0],
            ct * p[1] - st * p[0] - rx[1],
            p[2] - rx[2],
        )
        length = math.sqrt(sum(x * x for x in delta))
        tau = length / C
    return tau - sv.clock


def visibility(satellites, tow, rx):
    directions = np.stack([sv.position(tow - sv.toe) - rx for sv in satellites])
    directions /= np.linalg.norm(directions, axis=1)[:, None]
    sine = directions @ (rx / np.linalg.norm(rx))
    return directions, sine


def geometry(seed, n, height):
    rng = np.random.default_rng(seed)
    rx = receiver(0, height)[0]
    # Choose actual orbital planes and anomalies closest to well-separated sky
    # directions. All SVs and the receiver continue moving during the record.
    bank = [SpaceVehicle(1, *row) for row in rng.uniform(-math.pi, math.pi, (270, 2))]
    normed = np.array([sv.position(-5) - rx for sv in bank])
    normed /= np.linalg.norm(normed, axis=1)[:, None]
    chosen = []
    for i in range(n):
        az = 2 * math.pi * i / (n - 1)
        el = math.radians(78 if i == n - 1 else 18 + (i % 3) * 7)
        direction = [
            math.sin(el),
            math.cos(el) * math.cos(az),
            math.cos(el) * math.sin(az),
        ]
        idx = int(np.argmax(normed @ direction))
        base = bank[idx]
        chosen.append(SpaceVehicle(i + 1, angle_rad(base.node), angle_rad(base.m0)))
        normed[idx] = 0
    return chosen


class NativeMixer:
    """Optional fused source mixer. Arrays are native contiguous NumPy buffers."""

    def __init__(self, path):
        self.lib = ctypes.CDLL(str(Path(path).resolve()))
        self.call = self.lib.eph_sky_mix
        vector = np.ctypeslib.ndpointer
        self.call.argtypes = [
            ctypes.c_size_t,
            vector(np.float64, ndim=1, flags="C_CONTIGUOUS"),
            *([ctypes.c_double] * 5),
            vector(np.int8, ndim=1, flags="C_CONTIGUOUS"),
            vector(np.int8, ndim=1, flags="C_CONTIGUOUS"),
            vector(np.complex64, ndim=1, flags="C_CONTIGUOUS"),
            vector(np.complex64, ndim=1, flags=("C_CONTIGUOUS", "WRITEABLE")),
        ]
        self.call.restype = None

    def add(
        self, z, local, chip0, code_hz, phase, if_hz, amplitude, code, data, oscillator
    ):
        # The mixer directly indexes caller-owned buffers, including the final chip.
        if len(z) != len(local) or len(code) != 1023 or len(oscillator) != 2048:
            raise ValueError("invalid mixer buffer lengths")
        if (
            not len(z)
            or chip0 < 0
            or code_hz <= 0
            or not chip0 + code_hz * local[-1] < 20460 * len(data)
        ):
            raise ValueError("navigation bits do not cover the mixing span")
        self.call(
            len(z),
            local,
            chip0,
            code_hz,
            phase,
            if_hz,
            amplitude,
            code,
            data,
            oscillator,
            z,
        )


def path_bias_seconds(time_s, prn, slant_tecu, broadcast_error_m):
    """Illustrative independent range errors for code and carrier, in seconds."""
    angle = 2 * math.pi * (time_s / 1800 + prn * 0.38196601125)
    tec = slant_tecu * (1 + 0.5 * math.sin(angle))
    ionosphere_m = 40.3e16 * tec / L1**2
    common_m = broadcast_error_m * math.cos(angle)
    return (common_m + ionosphere_m) / C, (common_m - ionosphere_m) / C


def ground_delays(sin_el, zenith_m, reflector_height_m):
    """Return neutral and reflected excess delays in seconds for the visible sky."""
    # Black–Eisner mapping above the generator's 5° elevation mask.
    neutral = zenith_m * 1.001 / np.sqrt(0.002001 + sin_el**2)
    # Far-field path difference to the image antenna across a horizontal plane.
    excess = 2 * reflector_height_m * sin_el
    return neutral / C, excess / C


def mix_numpy(z, local, chip0, chip_hz, phase0, if_hz, amplitude, code, nav, table):
    chip = np.floor(chip0 + chip_hz * local).astype(np.int64)
    index = np.floor(2048 * (phase0 + if_hz * local)).astype(np.int64) & 2047
    z += amplitude * code[chip % 1023] * nav[chip // 20460] * table[index]


def generate(
    output,
    truth,
    seconds,
    count,
    height,
    seed,
    cn0,
    scenario=None,
    stress=None,
    mixer=None,
    slant_tecu=0.0,
    broadcast_error_m=0.0,
    zenith_delay_m=0.0,
    ground_echo=None,
):
    delays = (slant_tecu, broadcast_error_m, zenith_delay_m)
    if not all(math.isfinite(x) and x >= 0 for x in delays):
        raise ValueError("slant TEC, broadcast error and zenith delay must be finite and nonnegative")
    reflector_height_m, reflection = ground_echo if ground_echo is not None else (0.0, 0.0)
    if (
        not math.isfinite(reflector_height_m) or reflector_height_m < 0
        or not -1 <= reflection <= 1
    ):
        raise ValueError("ground echo needs a finite nonnegative height and voltage ratio in [-1, 1]")
    ground_model = bool(zenith_delay_m or reflection)
    if ground_model and (height or scenario):
        raise ValueError("ground propagation requires the static ground receiver")
    start_tow, week = START, WEEK
    phase = node = 0.0
    if scenario:
        try:
            prefix, alt, num = scenario.split("-")
            height = int(alt)
            num = int(num)
            if (
                prefix != "leo"
                or height not in SPEC["altitudes_km"]
                or not 1 <= num <= len(SPEC["starts"])
            ):
                raise ValueError
            cfg = SPEC["starts"][num - 1]
        except (ValueError, IndexError):
            raise ValueError("scenario must be a frozen name, e.g. leo-550-1") from None
        start_tow, week, phase = cfg["tow_s"], cfg["gps_week"], cfg["phase_deg"]
        node = SPEC["raan_deg"]
        toe = 16 * int(start_tow // 16)
        n = math.sqrt(MU / 26560000.0**3)
        satellites = [
            SpaceVehicle(
                1 + 4 * plane + slot,
                plane * math.pi / 3,
                slot * math.pi / 2 + plane * math.pi / 12 + n * toe,
                toe,
            )
            for plane in range(6)
            for slot in range(4)
        ]
    else:
        satellites = geometry(seed, count, height)
    ms_total = round(1000 * seconds)
    if ms_total < 1 or seconds > 2 * 3600 or not 4 <= count <= 8:
        raise ValueError(
            "choose 1 ms to 2 hours and 4 to 8 satellites for custom geometries"
        )
    if truth == Path("-"):
        raise ValueError("truth requires its own file")
    rng = np.random.default_rng(seed)
    # Include the last subframe before the receive start for signal flight time.
    begin = 30 * math.floor((start_tow - 0.15) / 30)
    nav = [sv.nav(week, begin, start_tow - begin + seconds + 1) for sv in satellites]
    sigma = 0.7
    table = np.exp(2j * np.pi * np.arange(2048) / 2048).astype(np.complex64)
    initial = receiver(0, height, phase, node)[0]
    anchor = np.array([clock_range(sv, start_tow, initial) for sv in satellites])
    sink = nullcontext(sys.stdout.buffer) if output == Path("-") else output.open("wb")

    effects = Stress(stress)
    clock_scale = effects.clock_scale
    t = np.arange(10 * SAMPLES_MS) / RATE / clock_scale
    if_physical = L1 - (L1 - IF_HZ) * clock_scale
    add = mixer.add if mixer is not None else mix_numpy
    prns = np.array([sv.prn for sv in satellites])
    threshold = math.sqrt(
        2 * sigma**2 * 10 ** (SPEC["qualification"]["min_cno_dbhz"] / 10) / RATE
    )

    def signal_levels(t_s, pos):
        directions, sine = visibility(satellites, start_tow + t_s, pos)
        zenith = pos / np.linalg.norm(pos)
        east = np.array([-zenith[1], zenith[0], 0.0])
        east /= np.linalg.norm(east)
        north = np.cross(zenith, east)
        azimuth = np.arctan2(directions @ east, directions @ north)
        cn0s = np.full(len(satellites), cn0)
        if scenario:
            low, high = SPEC["nominal_cno_dbhz"]
            cn0s = low + (high - low) * np.clip(sine, 0.0, 1.0)
        amplitudes = np.sqrt(2 * sigma**2 * 10 ** (cn0s / 10) / RATE)
        amplitudes[sine < math.sin(math.radians(5))] = 0
        amplitudes = effects.amplitudes(
            t_s, prns, np.arcsin(np.clip(sine, -1, 1)), amplitudes, azimuth
        )
        return directions, amplitudes

    def record(writer, t_s):
        pos, vel = receiver(t_s, height, phase, node)
        directions, amps = signal_levels(t_s, pos)
        visible = amps >= threshold
        design = np.column_stack((directions[visible], np.ones(sum(visible))))
        pdop = 999.0
        if len(design) >= 4:
            try:
                pdop = math.sqrt(np.trace(np.linalg.inv(design.T @ design)[:3, :3]))
            except np.linalg.LinAlgError:
                pass
        writer.writerow([t_s, *pos, *vel, sum(visible), pdop, week, start_tow + t_s])

    with sink as out, truth.open("w", newline="") as save:
        out.write(HEADER.pack(b"EIQ1", RATE, IF_HZ, 1, 0, ms_total * SAMPLES_MS))
        writer = csv.writer(save)
        writer.writerow([*TRUTH_HEADER, "gps_week", "tow_s"])
        delay = anchor.copy()
        for ms in range(0, ms_total, 10):
            size = min(10, ms_total - ms) * SAMPLES_MS
            local = t[:size]
            start = ms / (1000 * clock_scale)
            span = size / (RATE * clock_scale)
            p, v = receiver(start, height, phase, node)
            directions, amplitudes = signal_levels(start, p)
            if ms % 200 == 0:
                record(writer, ms / 1000)
            rx_end = receiver(start + span, height, phase, node)[0]
            if ground_model:
                sine = directions @ (p / np.linalg.norm(p))
                _, sine_end = visibility(satellites, start_tow + start + span, rx_end)
                neutral, excess = ground_delays(sine, zenith_delay_m, reflector_height_m)
                neutral_end, excess_end = ground_delays(
                    sine_end, zenith_delay_m, reflector_height_m
                )
            z = sigma * (
                rng.standard_normal(size, dtype=np.float32)
                + 1j * rng.standard_normal(size, dtype=np.float32)
            )
            visible = amplitudes > 0
            # Resolve shifts relative to the ADC sample clock. Signed LO
            # error and sample-rate error use the same TCXO ppm setting.
            for j in np.flatnonzero(visible):
                sv = satellites[j]
                if np.isnan(delay[j]):
                    delay[j] = clock_range(sv, start_tow + start, p)
                end = clock_range(sv, start_tow + start + span, rx_end)
                slope = (end - delay[j]) / span
                code_delay, carrier_delay = delay[j], delay[j]
                code_slope, carrier_slope = slope, slope
                if ground_model:
                    code_delay += neutral[j]
                    carrier_delay += neutral[j]
                    delay_rate = (neutral_end[j] - neutral[j]) / span
                    code_slope += delay_rate
                    carrier_slope += delay_rate
                if slant_tecu or broadcast_error_m:
                    c0, p0 = path_bias_seconds(
                        start, sv.prn, slant_tecu, broadcast_error_m
                    )
                    c1, p1 = path_bias_seconds(
                        start + span, sv.prn, slant_tecu, broadcast_error_m
                    )
                    code_delay += c0
                    carrier_delay += p0
                    code_slope += (c1 - c0) / span
                    carrier_slope += (p1 - p0) / span
                tx0 = start_tow - begin + start - code_delay
                first_chip = 1023000 * tx0
                chip_hz = 1023000 * (1 - code_slope)
                phase0 = (
                    (anchor[j] - carrier_delay) * L1
                    + 0.13 * j
                    + (if_physical * start) % 1
                ) % 1
                carrier_hz = if_physical - L1 * carrier_slope
                add(
                    z, local, first_chip, chip_hz, phase0, carrier_hz,
                    amplitudes[j], sv.code, nav[j], table,
                )
                if reflection:
                    # The echo carries the delayed navigation bits, spreading code
                    # and L1 phase. Changing elevation gives differential Doppler.
                    rate = (excess_end[j] - excess[j]) / span
                    add(
                        z,
                        local,
                        first_chip - 1023000 * excess[j],
                        chip_hz - 1023000 * rate,
                        (phase0 - L1 * excess[j]) % 1,
                        carrier_hz - L1 * rate,
                        reflection * amplitudes[j],
                        sv.code,
                        nav[j],
                        table,
                    )
                delay[j] = end
            delay[~visible] = np.nan
            out.write(pack(z))
        if ms_total % 200 == 0:
            final = ms_total / 1000
            record(writer, final)


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("output", type=Path)
    cli.add_argument("--truth", required=True, type=Path)
    cli.add_argument(
        "--scenario",
        help="frozen representative constellation and LEO orbit (e.g. leo-550-1)",
    )
    cli.add_argument(
        "--stress",
        choices=["nominal"] + [c["id"] for c in SPEC["stress"]],
        help="RF impairment from the frozen suite; receiver resets are applied by replay",
    )
    cli.add_argument("--seconds", type=float, default=25.0)
    cli.add_argument("--satellites", type=int, choices=range(4, 9), default=6)
    cli.add_argument(
        "--orbit-km",
        type=int,
        choices=[0, 400, 550, 800],
        default=0,
        help="0 selects a static equatorial ground receiver",
    )
    cli.add_argument("--seed", type=int, default=711)
    cli.add_argument(
        "--slant-tecu",
        type=float,
        default=0,
        help="mean slant TEC in an illustrative per-link variation",
    )
    cli.add_argument(
        "--broadcast-error-m",
        type=float,
        default=0,
        help="amplitude of a slowly varying line-of-sight orbit/clock residual",
    )
    cli.add_argument(
        "--zenith-delay-m", type=float, default=0,
        help="static ground receiver neutral-atmosphere zenith delay, in meters",
    )
    cli.add_argument(
        "--ground-echo", type=float, nargs=2, metavar=("HEIGHT_M", "VOLTAGE"),
        help="static ground echo from a flat plane below the antenna, signed voltage relative to direct",
    )
    cli.add_argument(
        "--cn0", type=float, default=53.0, help="per-SV nominal C/N0 in dB-Hz"
    )
    cli.add_argument(
        "--mixer", type=Path, help="optional compiled host-only sky mixer library"
    )
    a = cli.parse_args()
    if a.output.resolve() == a.truth.resolve():
        cli.error("truth and EIQ must have distinct paths")
    generate(
        a.output,
        a.truth,
        a.seconds,
        a.satellites,
        a.orbit_km,
        a.seed,
        a.cn0,
        a.scenario,
        a.stress,
        NativeMixer(a.mixer) if a.mixer else None,
        a.slant_tecu,
        a.broadcast_error_m,
        a.zenith_delay_m,
        a.ground_echo,
    )
