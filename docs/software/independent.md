# Independent baseband replay

A separate GPS signal model generated 65 seconds of L1 C/A samples for a stationary antenna at ECEF (6,378,137, 0, 0) m. GPS-SDR-SIM read the published January 1, 2022 broadcast ephemerides and produced the satellite signals, ranges, Doppler and LNAV words. The import step shifts these complex samples to +4.092 MHz, adds seeded Gaussian noise and applies the board's two-bit sign/magnitude quantizer. The receiver starts cold with only the EIQ sample stream. [GPS-SDR-SIM source and broadcast file](https://github.com/osqzss/gps-sdr-sim/tree/28ca29a6719475195e3aabd5930c4ed02d67190f)

| Host receiver | First valid fix | First of five truth-checked fixes | Valid epochs after first fix | 95th-percentile position / speed error |
| --- | ---: | ---: | ---: | ---: |
| Scalar | 33.2 s | 33.6 s | 160/160 | 0.685 m / 0.159 m/s |
| Half-chip sign correlator | 33.2 s | 33.6 s | 160/160 | 1.005 m / 0.167 m/s |

Both builds emitted all 325 status records, decoded the correct GPS week modulo 1024 and reported zero sample gaps. These measurements cover an offline, stationary waveform with historical satellite orbits. [Source, recording, executable and output hashes](measurements/independent-static.json)

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
