"""Export published GNSS-SDR v0.0.19 fixes for the 2013-04-04 CTTC recording.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import csv
from datetime import date, datetime
import struct
import sys
import xml.etree.ElementTree as ET

RECORD = struct.Struct("<II17dBBBff4d")
CAPTURE_DATE = date(2013, 4, 4)
GPS_EPOCH = date(1980, 1, 6)
LEAP_SECONDS = 16


def export(binary, gpx, output):
    """Match public GPX to unrounded RTKLIB ECEF and correct the receiver clock."""
    by_epoch = {}
    for row in RECORD.iter_unpack(binary.read()):
        raw_week, raw_receiver_tow_s = row[1:3]
        epoch = (raw_week, round(1000 * raw_receiver_tow_s))
        by_epoch.setdefault(epoch, []).append(row)

    w = csv.writer(output)
    w.writerow(("gps_week","tow_s","x_m","y_m","z_m","lat_deg","lon_deg",
                "ellipsoid_height_m","sv","rx_clock_offset_s","raw_rx_tow_s",
                "gps_week_dump","pdop"))
    last = None
    for pt in ET.parse(gpx).getroot().findall(".//{*}trkpt"):
        stamp = datetime.fromisoformat(pt.findtext("{*}time").replace("Z","+00:00"))
        if stamp.date() != CAPTURE_DATE:
            raise ValueError(f"Unexpected GPS era in GPX timestamp {stamp}")
        week, day = divmod((stamp.date()-GPS_EPOCH).days, 7)
        ms = ((day*86400+stamp.hour*3600+stamp.minute*60+stamp.second+LEAP_SECONDS)*1000
              + round(stamp.microsecond/1000))
        llh = (float(pt.attrib["lat"]), float(pt.attrib["lon"]), float(pt.findtext("{*}ele")))
        candidates = [r for r in by_epoch.get((week, ms), ())
                      if all(abs(a-b) <= 1e-10 for a,b in zip(r[16:19], llh))]
        if not candidates:
            raise ValueError(f"GPX fix has no equivalent binary PVT at {stamp}")
        r = candidates[-1]
        tow = r[2]-r[3]
        if last is not None and (week, tow) <= last:
            raise ValueError("Nonmonotonic GPS time")
        if r[20:22] != (5, 0) or r[19] < 4:
            raise ValueError("Expected a standalone ECEF fix")
        w.writerow((week,f"{tow:.9f}",*(round(v,8) for v in r[4:7]),
                    round(r[16],12),round(r[17],12),round(r[18],8),r[19],round(r[3],12),
                    round(r[2],9),r[1],r[-3]))
        last = week,tow
    if last is None:
        raise ValueError("GPX has no published fixes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=argparse.FileType("rb"))
    parser.add_argument("gpx", type=argparse.FileType("rb"))
    args = parser.parse_args()
    export(args.binary, args.gpx, sys.stdout)
