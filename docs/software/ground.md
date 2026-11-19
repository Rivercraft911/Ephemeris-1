# Find the antenna on the ground

The Pico solves its own position. The laptop reads the 5 Hz USB status stream and displays WGS 84 latitude, longitude and ellipsoid height. Each valid row includes the satellite count and PDOP. During acquisition the display shows how many satellites are locked and how many have usable navigation data. [Receiver status format](receiver.md#output-and-time) · [WGS 84 ellipsoid](https://earth-info.nga.mil/?action=wgs84&dir=wgs84)

1. With power off, wire J2 to the Pico, then power the assembled front end. Use the `ephemeris_pico.uf2` from the `pico-plus-2-ground-receiver` workflow artifact for the outdoor trial. It enables terrestrial neutral-atmosphere correction and Doppler-aided position smoothing. Use an L1 antenna with open sky and keep it still. Rev 1 has antenna bias disabled. An active antenna needs its own bias supply. [Firmware builds](https://github.com/Rivercraft911/Ephemeris-1/actions/workflows/receiver.yml) · [Power and pinout](../../firmware/README.md#wiring-the-live-front-end) · [RF path](../design.md)
2. Plug the Pico into the laptop. Install pyserial and run the viewer. With multiple attached units, use `--port PORT` to select the live receiver.

```sh
python3 -m pip install pyserial
python3 tools/ground_fix.py
```

A valid fix needs at least four satellites. A sample discontinuity, stalled clock, one second without fresh reports, or USB disconnect clears the coordinates. The viewer runs locally. To save a new private log, add `--record build/current/ground.csv`.

## Check against a known point

Use a surveyed antenna position or a reference receiver of known accuracy. Account for the antenna offset above the marker and use WGS 84 ellipsoid height. Start the log as close to power-on as practical. Measure elapsed wall time from power-on to first fix separately. The sample clock starts at capture and stops advancing if the clock input stops.

The proposed first outdoor milestone is five consecutive 200 ms fixes, each within 10 m horizontally and 20 m vertically of the antenna reference. Keep recording for five minutes after first fix and report horizontal, absolute vertical and 3D errors at the 95th percentile. The scorer uses latitude and longitude in decimal degrees and ellipsoid height in metres. It reads the reference only on the laptop during offline scoring. It prints aggregate errors and the longest passing streak.

```sh
python3 tools/ground_fix.py --replay build/current/ground.csv --reference LAT LON HEIGHT
```

On the 65-second independent static simulation, both receivers returned their first fix at sample time 33.2 s and held 160 consecutive on-target fixes. The half-chip build measured 0.739 m horizontal, 0.805 m absolute vertical and 1.005 m 3D at p95. The scalar reference measured 0.499 m, 0.594 m and 0.685 m respectively. The outdoor measurement awaits the assembled board. [Independent recording and hashes](independent.md)
