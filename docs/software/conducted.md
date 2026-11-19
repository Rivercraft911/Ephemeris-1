# Conducted LEO test

Connect the simulator RF output to the assembled Ephemeris 1 antenna input through a measured cable and attenuator. Connect the front end to the Pico. The receiver gets GPS L1 C/A through the antenna port and sends status over USB. The simulator, timing instrument and host hold all reference data. Results apply to the recorded board, firmware, RF level and motion profiles. [Pico wiring and bring-up](../../firmware/README.md) · [Frozen LEO suite](../../tests/scenarios.json)

### Set up the bench

Install NumPy and pyserial in the host Python environment. Build and flash the live image. Run `python3 -m serial.tools.list_ports -v` on the powered Pico to read its USB serial number. Record both boards' identities, the simulator hardware and software versions, the timing instrument and reference, the calibrated level at the SMA, cable delay and power-measurement basis. Preserve instrument exports with the run. The six strings below describe the required entries in a local `setup.json`. Replace each description with the bench measurement or identifier.

```json
{
  "rf_board_id": "PCB revision and assembly identifier",
  "pico_usb_serial": "USB serial reported by the Pico",
  "simulator_id": "Safran host and RF-output unit identifiers",
  "simulator_version": "installed Skydel version and RF configuration",
  "timing_reference": "instrument, common reference and trigger-delay calibration",
  "rf_calibration": "measured SMA input power, path loss, group delay, noise density and calibration record"
}
```

```sh
RUN=build/current/conducted
python3 tools/qualify.py prepare "$RUN" --firmware build/current/pico/ephemeris_pico.uf2 --setup setup.json
```

Preparation archives the exact image and suite, then writes one `run.json` under each of the 12 case directories. Use a fresh campaign directory after changing the hardware, firmware, calibration or simulator software. Keep the RF receiver unpowered until each cold start. Leave the front end enough time to settle before releasing the Pico. Count Pico power-on as elapsed time zero.

### Run a case

1. Read the case's `run.json`. Configure a circular receiver orbit of radius `6378137 m + altitude_km × 1000`, inclination 97.6°, ascending node 35° east in ECEF at power-on, and the specified argument of latitude. Set simulator GPS week and TOW to the values in the manifest at Pico power-on. Use the zenith antenna, 5° mask and nominal 40–48 dB-Hz. Record ephemerides, atmosphere, clock, power and antenna settings in the preset. At each altitude, start 1 runs one orbital period and starts 2–4 run 150 s.
2. Enable receiver and per-satellite L1 C/A raw logging in Skydel at 5 Hz, or an integer multiple that includes the 5 Hz grid at power-on. Enable RF output. Record receiver antenna ECEF coordinates and rotating-frame velocity, satellite ECEF coordinates, GPS week/TOW, health and C/N₀ or calibrated relative power. Save the simulator event log and its underrun count. [Skydel raw-log fields and logging settings](https://safran-navigation-timing.com/online-manuals/skydel/Content/SDX/Skydel/Settings-Global.htm)
3. Arm the scope/counter on the simulator time reference, Pico power-on/reset, GP7 sample-zero marker and GP6 sample clock. Capture the first sample edge and count actual ADC edges to a second independently timed observation near the end of the run. Apply the measured trigger and fixed RF path delays when expressing both observations on the simulator's reception-time axis. Enter the corresponding pair or pairs in `timing.csv` with the header `sample,elapsed_s`. Preserve the raw timing trace in the run archive. The scorer derives `rate = Δsample / Δtime` and `offset = elapsed_s − sample/rate`. A 1 ms alignment error moves a 7.5 km/s receiver by 7.5 m.
4. Start the collector before switching on Pico power at the scheduled simulator epoch. Leave the RF running until the collector gets the final capture counters. Repeat with a physical power cycle at every start.

```sh
CASE=leo-550-2
python3 tools/qualify.py collect "$RUN" "$CASE"
```

The collector waits for the selected Pico USB identity, records its serial as a comment, preserves the CDC bytes, and requests `stats` once enough ADC epochs have elapsed. An interrupted capture stays in `receiver.partial`. Keep that evidence and record a new run for qualification.

### Archive the simulator files

Create a ZIP in the case directory called `simulator.zip`. Include the saved preset and any external navigation/model inputs, the simulator run log, original receiver and satellite CSVs, RF and timing records, and a `truth-map.json` at the ZIP root. Paths inside the map are relative to the ZIP root. Use the actual column names from the installed simulator. The example shows one satellite entry; include every simulated GPS L1 C/A satellite with its actual PRN.

```json
{
  "preset": "scenario.sdx",
  "run_log": "logs/run.log",
  "receiver": "raw/receiver.csv",
  "columns": {
    "gps_week": "GPS Week Number", "tow_s": "GPS TOW (s)",
    "x_m": "ECEF X (m)", "y_m": "ECEF Y (m)", "z_m": "ECEF Z (m)",
    "vx_mps": "Velocity X (m/s)", "vy_mps": "Velocity Y (m/s)", "vz_mps": "Velocity Z (m/s)",
    "relative_db": "Relative Power Level (dB)"
  },
  "satellites": [
    {"prn": 3, "file": "raw/G03_L1CA.csv", "healthy": true, "reference_cn0_dbhz": 45.0}
  ]
}
```

Each `reference_cn0_dbhz` is the calibrated C/N₀ for that signal at zero relative power. Replace the example value using the actual RF configuration. If the raw log supplies absolute C/N₀, map its column to `cn0_dbhz` and omit the relative-power fields. Use `healthy` for a known constant simulated SV health state, or map `health` to a timestamped status column whose zero value means healthy. The converter selects healthy signals at ≥40 dB-Hz above the mask and calculates PDOP from their line-of-sight vectors. Invalid geometry is represented by PDOP 999.

```sh
python3 tools/qualify.py truth "$RUN" "$CASE"
python3 tools/qualify.py seal "$RUN" "$CASE" --rf-underruns "$RF_UNDERRUNS"
python3 tools/qualify.py score "$RUN" > "$RUN/report.json"
```

Set `RF_UNDERRUNS` to the count from that case's simulator run log. Seal each of the 12 nominal cases before scoring the complete directory. The report verifies the image, suite, setup and evidence hashes, recomputes the truth table from archived raw logs, checks the reported GPS time and verifies the motion falls in the declared orbital case. It scores the complete independent 5 Hz grid, including invalid and missing eligible epochs. Nonzero DMA losses, FIFO stalls, clock timeouts, USB record losses, RF underruns or restored warm state fail a nominal run. The report includes the measured time base, observed hardware counters, time to first fix, availability, and position and velocity errors. Retain the complete directory so another engineer can regenerate the report.

The nominal report covers acquisition and endurance. The separately specified fades, outages, oscillator offsets and resets need their own labeled runs. Preserve each capture segment's timing observations across a reset. [Receiver acceptance criteria](receiver.md#leo-acceptance)
