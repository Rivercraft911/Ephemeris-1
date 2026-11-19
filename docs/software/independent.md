# Independent baseband replay

A separate GPS signal model generated two L1 C/A recordings. One runs 65 seconds at a fixed ECEF position of (6,378,137, 0, 0) m. The second runs 90 seconds on a 550 km circular orbit inclined at 97.6°. GPS-SDR-SIM read the published January 1, 2022 broadcast ephemerides and produced the satellite signals, ranges, Doppler and LNAV words. The import step shifts these complex samples to +4.092 MHz, adds seeded Gaussian noise and applies the board's two-bit sign/magnitude quantizer. The receiver starts cold with only the EIQ sample stream. [GPS-SDR-SIM source and broadcast file](https://github.com/osqzss/gps-sdr-sim/tree/28ca29a6719475195e3aabd5930c4ed02d67190f)

| Motion and host receiver | First valid fix | First of five truth-checked fixes | Valid epochs after first fix | 95th-percentile position / velocity error |
| --- | ---: | ---: | ---: | ---: |
| Static, scalar | 33.2 s | 33.6 s | 160/160 | 0.685 m / 0.159 m/s |
| Static, half-chip | 33.2 s | 33.6 s | 160/160 | 1.005 m / 0.167 m/s |
| 550 km, scalar | 33.2 s | 33.2 s | 285/285 | 0.561 m / 0.172 m/s |
| 550 km, half-chip | 33.2 s | 33.2 s | 285/285 | 0.829 m / 0.172 m/s |

Both builds emitted every 5 Hz status record, decoded the correct GPS week modulo 1024 and reported zero sample gaps. The orbital run uses 10 Hz simulator motion inputs and independently evaluates ECEF position and velocity at each reported sample time. [Static measurements and hashes](measurements/independent-static.json) · [Orbital measurements and hashes](measurements/independent-orbit.json)

### Cold search at a fixed work rate

A blind PRN 1 search on the Pico took 10.628 seconds for 1,141 work steps. The table gives the half-chip receiver the same average search budget on both 165-second recordings. Times are measured from sample zero to the first 5 Hz report showing each milestone. The ground build searches ±12 kHz first. The orbital build starts at ±70 kHz. Both refresh an old candidate on a new set of samples before confirming lock.

| Recording and search | First lock | Four locks | First position | Position / velocity error, 95th percentile |
| --- | ---: | ---: | ---: | ---: |
| Ground, previous wide search | 10.8 s | 74.4 s | 99.2 s | 1.457 m / 0.244 m/s |
| Ground, current ground search | 2.4 s | 15.0 s | 39.2 s | 1.211 m / 0.171 m/s |
| 550 km, previous wide search | 10.8 s | 106.2 s | No fix in 165 s | — |
| 550 km, current wide search | 11.0 s | 87.2 s | 123.2 s | 1.588 m / 0.255 m/s |

Across four ground power-up offsets, first position took 39.2–50.2 seconds with the current build and 99.2–112.2 seconds with the previous build. In the orbital case, five consecutive truth-checked reports began at 124.6 seconds. That misses the 120-second target and the 0.2 m/s velocity target at this work rate. The 825 status epochs on each 165-second run had zero sample gaps. Every PRN reported locked was present in the generator's transmitted set at that epoch. [Results, input hashes and PRN allocations](measurements/cold-replay.json)

The optimized wide sweep took 5.706271 seconds for the same 1,141 steps on the Pico. Applying `--work 1141/5706` to both native profiles gave a ground first lock at 1.2 seconds, four locks at 7.8 seconds and position at 39.2 seconds. The 550 km receiver reached those milestones at 6.0, 46.4 and 69.2 seconds. It returned 480 consecutive fixes at 5 Hz, with 95th-percentile errors of 1.880 m and 0.234 m/s. Ground timing uses the measured wide-search average as an estimate for the narrow pass. [Paced replay results](measurements/cold-replay.json)

### Five-report velocity fit

`-DEPHEMERIS_VELOCITY_FIT=ON` fits a line through five successive ECEF Doppler velocities and evaluates it at the current epoch. At 5 Hz the window spans 0.8 seconds. Its endpoint weights, oldest first, are `[-1, 0, 1, 2, 3] / 5`. They sum to one and their weighted time offsets sum to zero, so constant acceleration introduces zero lag. Constant jerk contributes a velocity error of −0.04 s² times the jerk. A sharp maneuver can therefore produce a transient overshoot. The first four reports after startup, an invalid solution or a sampling break carry single-epoch velocity. The position smoother consumes the raw Doppler velocity before this optional fit.

Enabling the fit adds 112 bytes to the RP2350 receiver object. The velocity update function occupies 416 bytes in the Arm GCC 14.2 release object.

| 165-second recording | First position | Position error p95 | Single-epoch velocity error p95 | Fitted velocity error p95 |
| --- | ---: | ---: | ---: | ---: |
| Static ground | 39.2 s | 1.095 m | 0.166 m/s | 0.141 m/s |
| 550 km | 69.2 s | 1.880 m | 0.234 m/s | 0.145 m/s |
| 400 km, held out | 99.2 s | 9.955 m | 1.584 m/s | 0.895 m/s |
| 800 km, held out | 69.2 s | 2.121 m | 0.294 m/s | 0.168 m/s |

Both builds used `--work 1141/5706` on the same I/Q for each pair. The 400 km run missed the 0.2 m/s velocity threshold. Its first 90 fixes used four satellites with mean PDOP 24.63. Its final 180 fixes used eight satellites with mean PDOP 2.58 and fitted velocity error of 0.153 m/s at the 95th percentile. To regenerate the held-out arcs, pass `--altitude-km 400 --phase-deg 103` or `--altitude-km 800 --phase-deg 211` to the motion exporter and scorer. Use import seeds 7411 and 9203, respectively. [Paired hashes, geometry and numerical checks](measurements/velocity-fit.json)

### Full-band strong-signal survey

`-DEPHEMERIS_FLIGHT_SURVEY=ON` tests each unoccupied PRN across ±70 kHz with one millisecond of data. A correlation peak at least 18 times the mean code-cell power goes to the usual independent confirmation. The next pass uses four dwells for every remaining PRN while a tracking slot is available. This option defaults to off. Cached Doppler searches and fresh searches around an old detection use four dwells. Enabling ground-first search as well inserts a four-dwell ±12 kHz pass before each wide pass.

| Orbit | Eight tracking locks, baseline → survey | First position, baseline → survey | Survey position / velocity error, p95 |
| --- | ---: | ---: | ---: |
| 400 km | 109.8 → 35.0 s | 99.2 → 57.2 s | 2.638 m / 0.231 m/s |
| 550 km | 127.6 → 52.2 s | 69.2 → 69.2 s | 1.009 m / 0.106 m/s |
| 800 km | 75.6 → 42.2 s | 69.2 → 39.2 s | 1.575 m / 0.148 m/s |

Each pair replayed the same 165 seconds of I/Q on the host with the five-report velocity fit and `--work 1141/5706`. The full-band flight survey releases a tracking slot when a parity-checked LNAV subframe 1 reports nonzero health. It can retry that satellite after 60 s. At 400 km, PRNs 22 and 28 broadcast unhealthy status. Their replacement, PRN 32, locked at 93.0 s and supplied a usable ephemeris at 123.2 s. Releasing those slots changed p95 velocity error from 0.239 to 0.231 m/s and position error from 2.539 to 2.638 m. Across the 210 reports using all eight satellites from 123.2 s, p95 velocity error was 0.198 m/s. The full-arc result still exceeds the 0.2 m/s threshold. At 550 and 800 km the eighth usable satellites were PRN 4 and PRN 29 respectively. [Recordings, work counts, transmitted-PRN checks and output hashes](measurements/flight-survey.json)

The first pass favors strong signals. At 38, 40 and 45 dB-Hz it confirmed 0, 0 and 43 of 64 trials. The four-dwell search confirmed 14, 49 and 64. Run `signal_test --sweep` and `signal_test --cross-code` from a survey build to reproduce the native sensitivity and single-transmitter controls. The normal test suite also checks a weak PRN recovered on the second pass, a vanished strong signal, and two simultaneous signals 18 dB apart.

### Replay the recording

The adapter accepts interleaved signed 8-bit I followed by Q, sampled at 16.368 MHz in complex baseband. Input length must be an integral number of milliseconds. Amplitude is expressed in ADC threshold units per input count. The published run used gain 0.012 and Gaussian noise with standard deviation 0.8 on each axis.

```sh
python3 tools/import_baseband.py recording.iq8 recording.eiq --gain .012 --noise-sigma .8 --seed 10209
build/current/native/eph-replay recording.eiq --work 8 > receiver.csv
python3 tests/independent_static.py receiver.csv
```

The last command scores this specific known position and recording epoch. To select the half-chip implementation, configure a separate release build with `-DEPHEMERIS_COARSE_TRACKING=ON` and replay the same EIQ file.

`--work N/D` permits N acquisition steps per D milliseconds of input. The worker spends each whole step when it becomes available. Unused whole steps expire if it is waiting for the next sample block. For example, `--work 1141/10628` models the average rate of a measured blind PRN sweep on the Pico. A fixed rate approximates search throughput. DMA interrupts, tracker load and differences between search steps affect the actual board timing.

### Recreate the reference input

The optional external generator writes a baseband file locally. Use GPS-SDR-SIM revision `28ca29a6719475195e3aabd5930c4ed02d67190f` and its bundled `brdc0010.22n`. With `SIM` set to that checkout, the commands below reproduce the input. Upstream starts its output loop at the second 0.1-second trajectory point, so the 65.1-second setting yields exactly 1,063,920,000 complex samples.

```sh
cc -O3 "$SIM/gpssim.c" -lm -o build/current/gps-sdr-sim
build/current/gps-sdr-sim -e "$SIM/brdc0010.22n" -c 6378137,0,0 -t 2022/01/01,02:00:09 -s 16368000 -b 8 -i -d 65.1 -o recording.iq8
```

For the orbital recording, export 901 trajectory points and use the simulator's ECEF-motion input. The two-body trajectory starts at argument of latitude 12° and ECEF ascending node 35°. The frozen suite covers other starts and longer arcs. [LEO acceptance](receiver.md#leo-acceptance)

```sh
python3 tests/independent_orbit.py --motion orbit.csv
build/current/gps-sdr-sim -e "$SIM/brdc0010.22n" -u orbit.csv -t 2022/01/01,02:00:09 -s 16368000 -b 8 -i -d 90.1 -o orbit.iq8
python3 tools/import_baseband.py orbit.iq8 - --gain .012 --noise-sigma .8 --seed 10209 | build/current/native/eph-replay - --work 8 > orbit-receiver.csv
python3 tests/independent_orbit.py --score orbit-receiver.csv
```

For the 165-second cold-start trials, set the generator duration to `-d 165.1`. For the orbital case, first export the longer trajectory using `python3 tests/independent_orbit.py --motion orbit-165.csv --seconds 165`. Import with the same noise seed and pass `--seconds 165` to the appropriate scorer. Adding `-v` to the generator records its transmitted PRNs at startup and at each subsequent allocation. The first 65 seconds of static samples and 90 seconds of orbital samples match the original recordings byte for byte.
