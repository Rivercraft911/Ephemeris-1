# Independent baseband replay

A separate GPS signal model generated two L1 C/A recordings. One runs 65 seconds at a fixed ECEF position of (6,378,137, 0, 0) m. The second runs 90 seconds on a 550 km circular orbit inclined at 97.6°. GPS-SDR-SIM read the published January 1, 2022 broadcast ephemerides and produced the satellite signals, ranges, Doppler and LNAV words. The import step shifts these complex samples to +4.092 MHz, adds seeded Gaussian noise and applies the board's two-bit sign/magnitude quantizer. The receiver starts cold with only the EIQ sample stream. [GPS-SDR-SIM source and broadcast file](https://github.com/osqzss/gps-sdr-sim/tree/28ca29a6719475195e3aabd5930c4ed02d67190f)

| Motion and host receiver | First valid fix | First of five truth-checked fixes | Valid epochs after first fix | 95th-percentile position / velocity error |
| --- | ---: | ---: | ---: | ---: |
| Static, scalar | 33.2 s | 33.6 s | 160/160 | 0.685 m / 0.159 m/s |
| Static, half-chip | 33.2 s | 33.6 s | 160/160 | 1.005 m / 0.167 m/s |
| 550 km, scalar | 33.2 s | 33.2 s | 285/285 | 0.561 m / 0.172 m/s |
| 550 km, half-chip | 33.2 s | 33.2 s | 285/285 | 0.829 m / 0.172 m/s |

Both builds emitted every 5 Hz status record, decoded the correct GPS week modulo 1024 and reported zero sample gaps. The orbital run uses 10 Hz simulator motion inputs and independently evaluates ECEF position and velocity at each reported sample time. [Static measurements and hashes](measurements/independent-static.json) · [Orbital measurements and hashes](measurements/independent-orbit.json)

### Replay the recording

The adapter accepts interleaved signed 8-bit I followed by Q, sampled at 16.368 MHz in complex baseband. Input length must be an integral number of milliseconds. Amplitude is expressed in ADC threshold units per input count. The published run used gain 0.012 and Gaussian noise with standard deviation 0.8 on each axis.

```sh
python3 tools/import_baseband.py recording.iq8 recording.eiq --gain .012 --noise-sigma .8 --seed 10209
build/current/native/eph-replay recording.eiq --work 8 > receiver.csv
python3 tests/independent_static.py receiver.csv
```

The last command scores this specific known position and recording epoch. To select the half-chip implementation, configure a separate release build with `-DEPHEMERIS_COARSE_TRACKING=ON` and replay the same EIQ file.

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
