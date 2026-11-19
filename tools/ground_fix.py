"""Display an Ephemeris 1 ground fix over USB or replay a receiver status CSV.
SPDX-License-Identifier: Apache-2.0
"""

import argparse
from contextlib import ExitStack
import math
from pathlib import Path
import sys
import time

from iqio import RATE

A = 6378137.0
FLATTENING = 1 / 298.257223563
E2 = FLATTENING * (2 - FLATTENING)
STALE_S = 1.0  # five missed 5 Hz status periods
FIELDS = {"sequence", "sample", "gaps", "locked_mask", "nav_mask", "valid",
          "x_m", "y_m", "z_m", "sv", "pdop"}


def geodetic(x, y, z):
    """Return WGS 84 geodetic latitude and longitude in degrees, ellipsoid height in metres."""
    if not all(map(math.isfinite, (x, y, z))) or x == y == z == 0:
        raise ValueError("invalid ECEF position")
    p = math.hypot(x, y)
    lon = math.atan2(y, x)
    lat = math.atan2(z, p * (1 - E2))
    for _ in range(8):
        s = math.sin(lat)
        n = A / math.sqrt(1 - E2 * s * s)
        updated = math.atan2(z + E2 * n * s, p)
        delta = abs(updated - lat)
        lat = updated
        if delta < 1e-15:
            break
    s = math.sin(lat)
    height = p * math.cos(lat) + z * s - A * math.sqrt(1 - E2 * s * s)
    return math.degrees(lat), math.degrees(lon), height


class Display:
    def __init__(self):
        self.fields = []
        self.sequence = self.sample = self.gaps = None
        self.last_report = None
        self.position = None

    def update(self, line, now):
        """Return a display line, or None for optional diagnostic records."""
        if not line or line.startswith("#"):
            return None
        parts = line.split(",")
        self.position = None
        if parts[0] == "sequence":
            self.fields = parts if FIELDS.issubset(parts) and len(parts) == len(set(parts)) else []
            self.sequence = self.sample = self.gaps = self.last_report = None
            return "Waiting for receiver status" if self.fields else "Unknown receiver header"
        if not self.fields or len(parts) != len(self.fields):
            return "No fix | incomplete receiver record"
        row = dict(zip(self.fields, parts))
        try:
            sequence, sample, gaps, locked, nav, valid, sv = (
                int(row[name]) for name in ("sequence", "sample", "gaps", "locked_mask",
                                           "nav_mask", "valid", "sv")
            )
            if (not 0 <= sequence < 2**32 or sample < 0 or gaps < 0
                    or not 0 <= locked < 2**32 or not 0 <= nav < 2**32
                    or valid not in (0, 1)):
                raise ValueError("invalid status values")
            if self.sequence is not None:
                advance = (sequence - self.sequence) % 2**32
                if not 0 < advance < 2**31 or sample < self.sample or gaps < self.gaps:
                    return "No fix | stale receiver record"
                if gaps != self.gaps:
                    self.sequence, self.sample, self.gaps = sequence, sample, gaps
                    self.last_report = now
                    return f"No fix | sample discontinuity | gaps {gaps}"
                if sample == self.sample:
                    self.sequence = sequence
                    return "No fix | sample clock stopped"
            self.sequence, self.sample, self.gaps = sequence, sample, gaps
            self.last_report = now
            if not valid:
                return f"Searching | {locked.bit_count()} locked | {nav.bit_count()} with navigation data"
            xyz = tuple(float(row[key]) for key in ("x_m", "y_m", "z_m"))
            pdop = float(row["pdop"])
            if not 4 <= sv <= 32 or not math.isfinite(pdop) or pdop <= 0:
                raise ValueError("invalid solution geometry")
            lat, lon, height = geodetic(*xyz)
            self.position = xyz
        except (ValueError, OverflowError):
            return "No fix | invalid receiver record"
        return (f"Fix | lat {lat:.7f}° | lon {lon:.7f}° | ellipsoid {height:.2f} m"
                f" | {sv} SV | PDOP {pdop:.2f} | sample time {sample / RATE:.1f} s")

    def expire(self, now):
        if self.last_report is not None and now - self.last_report >= STALE_S:
            self.position = self.last_report = None
            return "No fix | no fresh receiver status"
        return None


def watch(port, display, show, log=None, clock=time.monotonic):
    fragment = bytearray()
    dropping = False
    try:
        while True:
            chunk = port.read_until(b"\n", 512)
            now = clock()
            if log and chunk:
                log.write(chunk)
            if chunk:
                if len(fragment) + len(chunk) > 512:
                    dropping = True
                    fragment.clear()
                    display.position = None
                    show("No fix | oversized receiver record")
                if not dropping:
                    fragment.extend(chunk)
                if chunk.endswith(b"\n"):
                    if not dropping:
                        show(display.update(fragment.decode("ascii", errors="replace").strip(), now))
                    dropping = False
                    fragment.clear()
            show(display.expire(now))
    except OSError:
        display.position = None
        show("No fix | USB disconnected")
        raise


def score_site(lines, reference):
    """Measure local tangent-plane errors against an independently known antenna position."""
    import numpy as np

    lat, lon, height = reference
    if not (-90 <= lat <= 90 and -180 <= lon <= 180 and math.isfinite(height)):
        raise ValueError("reference must contain geodetic latitude, longitude and ellipsoid height")
    lat, lon = math.radians(lat), math.radians(lon)
    up = (math.cos(lat) * math.cos(lon), math.cos(lat) * math.sin(lon), math.sin(lat))
    east = (-math.sin(lon), math.cos(lon), 0.0)
    north = np.cross(up, east)
    n = A / math.sqrt(1 - E2 * up[2] ** 2)
    site = np.array(((n + height) * up[0], (n + height) * up[1],
                     (n * (1 - E2) + height) * up[2]))
    view = Display()
    errors = []
    streak = longest = 0
    first = last = None
    for line in lines:
        if view.update(line.strip(), 0) is None:
            continue
        if view.position is None:
            streak, last = 0, None
            continue
        if first is None:
            first = view.sample / RATE
        delta = np.asarray(view.position) - site
        horizontal = math.hypot(delta @ east, delta @ north)
        vertical = abs(delta @ up)
        errors.append((horizontal, vertical, np.linalg.norm(delta)))
        if horizontal <= 10 and vertical <= 20:
            streak = streak + 1 if last is not None and view.sample - last == RATE // 5 else 1
            longest = max(longest, streak)
        else:
            streak = 0
        last = view.sample
    return dict(first_sample_s=first, fixes=len(errors), longest=longest,
                p95=np.percentile(errors, 95, axis=0) if errors else None)


class Terminal:
    def __init__(self):
        self.width = 0

    def show(self, message):
        if message is None:
            return
        if sys.stdout.isatty():
            print("\r" + message.ljust(self.width), end="", flush=True)
            self.width = len(message)
        else:
            print(message, flush=True)

    def finish(self):
        if self.width:
            print()


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    source = cli.add_mutually_exclusive_group()
    source.add_argument("--port", help="USB serial port (default: find a single Ephemeris 1)")
    source.add_argument("--replay", type=Path, help="read a saved receiver CSV instead of USB")
    cli.add_argument("--record", type=Path, help="save USB status to a new local file")
    cli.add_argument("--reference", nargs=3, type=float, metavar=("LAT_DEG", "LON_DEG", "HEIGHT_M"),
                     help="score a replay against a known WGS 84 antenna position")
    args = cli.parse_args()
    if args.replay and args.record:
        cli.error("--record requires USB input")
    if args.reference and not args.replay:
        cli.error("--reference requires a saved --replay")
    display, terminal = Display(), Terminal()
    try:
        if args.replay:
            with args.replay.open(encoding="ascii") as status:
                if args.reference:
                    result = score_site(status, args.reference)
                    if not result["fixes"]:
                        print("No valid fixes in recording")
                        return 1
                    h, v, d = result["p95"]
                    print(f"Valid fixes {result['fixes']} | first fix at sample time "
                          f"{result['first_sample_s']:.1f} s | longest on-target 5 Hz run "
                          f"{result['longest']} | p95 horizontal {h:.3f} m, "
                          f"absolute vertical {v:.3f} m, 3D {d:.3f} m")
                    return 0 if result["longest"] >= 5 else 1
                for line in status:
                    terminal.show(display.update(line.strip(), time.monotonic()))
            return 0
        import serial
        from serial.tools import list_ports

        device = args.port
        if not device:
            ports = [p.device for p in list_ports.comports()
                     if (p.vid, p.pid) == (0xcafe, 0x4011)]
            if len(ports) != 1:
                cli.error(f"found {len(ports)} Ephemeris ports; use --port for the live receiver")
            device = ports[0]
        with ExitStack() as stack:
            port = stack.enter_context(serial.Serial(device, 115200, timeout=0.2))
            log = stack.enter_context(args.record.open("xb", buffering=0)) if args.record else None
            terminal.show("Waiting for receiver status")
            watch(port, display, terminal.show, log)
    except KeyboardInterrupt:
        return 0
    except (ImportError, OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 1
    finally:
        terminal.finish()


if __name__ == "__main__":
    raise SystemExit(main())
