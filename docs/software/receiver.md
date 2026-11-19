# GPS L1 receiver

Ephemeris 1 uses a MAX2769B for RF and a Pimoroni Pico Plus 2 for baseband and navigation. The input is a continuous four-bit bus. Once configured, the ADC keeps sending samples through acquisition, tracking, and USB disconnects. This is the constraint that determines the firmware layout. [Board interface and startup](../design.md#first-capture) · [Pico build and bench instructions](../../firmware/README.md)

[Measured accuracy and Pico timing](results.md) · [Ground fix over USB](ground.md) · [Conducted Safran tests](conducted.md)

## Build and replay

Use CMake 3.20+, a C++17 compiler, Python 3 with NumPy, and Ninja. The host tools and native tests use the same receiver core as the Pico.

```sh
cmake -S . -B build/current/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/current/native
ctest --test-dir build/current/native --output-on-failure

python3 tools/vectors.py build/current/first.eiq --ms 250 --prn 1 --doppler -18320
build/current/native/eph-replay build/current/first.eiq --channels build/current/channels.csv
```

The last command searches an unknown signal and emits 5 Hz CSV to stdout. Expect a tracked channel and an invalid navigation solution from this one-satellite fixture. Set `--work` to choose acquisition steps per replay millisecond. The board runs the same steps on its second execution context. Native replay throughput depends on the host. Use `--seed PRN,DOPPLER_HZ,PHASE_CHIPS` only for an explicitly aided lab test.

### Wire format

Each EIQ1 file begins with a 32-byte little-endian header. It contains the four ASCII bytes `EIQ1`, the sample rate (unsigned 32 bits, 16,368,000 Hz), nominal IF (signed 32 bits, +4,092,000 Hz), encoding (unsigned 32 bits, 1), first ADC sample index (unsigned 64 bits), and total complex sample count (unsigned 64 bits). The payload consists of complete milliseconds. In each byte, the low nibble arrives first. Bits 3 through 0 are I sign, I magnitude, Q sign, Q magnitude. The sign/magnitude levels 00, 01, 10, 11 decode to +1, +3, −1, −3. A 10 ms recording takes 81,840 payload bytes. Start another segment after a discontinuity. [MAX2769B ADC format](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf)

### Output and time

Each status record includes the common raw sample epoch, cumulative discontinuities, active/locked/navigation PRN masks, validity, week modulo 1024, GPS seconds of week, ECEF position in meters and velocity in m/s, clock bias in meters and drift in m/s, satellite count, and PDOP. Bit zero of each mask denotes PRN 1. Clock bias refers to the arbitrary local receiver clock initialized at the first solution opportunity. The receiver never asserts an absolute calendar era. The channel diagnostic CSV can be enabled separately in replay. [GPS week rollover](https://www.gps.gov/news/gps-week-number-rollover)

## Signal processing decisions

| Research question | Implementation and experiment |
| --- | --- |
| How much work does sample formatting cost? | At 16.368 MS/s the IF advances one quarter-turn per sample. Swap I/Q and signs, then sum four samples into a common 4.092 MS/s complex stream. The rectangular filter is inexpensive and has a known group delay. Compare its measured sensitivity and timed correlations with a longer FIR before changing the flight path. The Pico bench also captures sign and magnitude on four synchronized PIO state machines and compares against nibble capture. The experimental word-parallel correlator consumes this layout directly. [Clements et al.](https://radionavlab.ae.utexas.edu/images/stories/files/papers/Optimized-Bit-Packing-for-Bit-Wise-Software-Defined-GNSS-Radio.pdf) |
| How does a cold receiver search orbital Doppler? | Search PRNs 1–32 across ±70 kHz, in bounded work units. The margin comes from line-of-sight speed divided by the 0.1903 m L1 wavelength, plus reference error. One FFT of a sampled dwell supplies multiple frequency bins by index rotation. Fold the linear correlation into the actual GPS code period. Verify a candidate on fresh data. Compare each optimized result with direct time-domain correlation. [Acquisition methods](https://www.ion.org/publications/abstract.cfm?articleID=9020) · [CubeSat cold-start flight experience](https://doi.org/10.1155/2020/8894984) |
| Which tracking results become measurements? | Track code and carrier separately for each satellite. Early/prompt/late accumulators span received code periods, including periods that cross DMA blocks. Fractional code-boundary timestamps and continuous carrier Doppler feed a common reception epoch. Measure Doppler jitter and range error as well as CPU time. [GNSS-SDR tracking reference](https://gnss-sdr.org/docs/sp-blocks/tracking/) |
| How does a fix get its time? | Synchronize 20 ms data bits, verify word parity and HOW time, then combine healthy subframes 1–3 with matching ephemeris issues. Evaluate broadcast orbits at transmit time, correct satellite clocks and Earth rotation, and solve position, velocity and receiver clock terms. A discontinuity invalidates the solution. Reacquisition must establish a fresh time anchor. [GPS interface specification](https://www.gps.gov/interface-control-documents-icds-interface-specifications-iss) |
| Which low-level tricks carry over from DVB? | Reuse the four-pin PIO input and DMA architecture, then test it with an independent clock. Use the same processing function for recorded and live blocks. Evaluate lookup tables, packed arithmetic and assembly with capture and background acquisition active. [Pico ingress measurements](https://github.com/Rivercraft911/RP2350_IQ_Benchmark/blob/main/results/ingress-wide/2026-10-16/README.md) · [ESP receiver replay](https://github.com/Rivercraft911/esp-dvbs2/blob/main/firmware/rx/main/replay.c) |

### Acquisition and tracking experiments

The cold search sums four independent 1 ms correlation-power surfaces. The code lasts exactly 2,046 acquisition samples. Zero-padding to a 4,096-point FFT computes a linear correlation, so fold its tail at lag 2,046 to obtain the cyclic GPS code correlation. Each Doppler bin shifts the stored data spectrum. Carrier-aided code-phase alignment compensates for the actual timestamp separation between dwells. Four fresh epochs check the candidate at three finer Doppler offsets. `signal_test --sweep` measures detection probability and false confirmations in a specified pre-quantization complex AWGN model.

The tracking loops accumulate between successive code boundaries. Early and late replicas integrate the local code over the same four ADC sample times as the input. Their shared fractional weight addresses the delay bias of a stepped replica. For navigation, smooth code boundaries against carrier-predicted increments with a four-second time constant. Estimate Doppler by differentiating a quadratic fit to three successive 160 ms means of unwrapped carrier phase. The acceleration term estimates velocity at the current measurement epoch. Verify these observables during fades and dynamics using independent truth.

The word-parallel tracker uses each sample's original I and Q sign and magnitude. A 32-sample signed sum is the total magnitude minus twice the magnitude selected by the XOR of the sample and replica signs. A four-chip window supplies all three replica masks. A deterministic dither interpolates the early and late replicas between raw samples. The carrier rotates once at each word center, including partial words at C/A period boundaries. The calculated boxcar amplitude gain at 70 kHz is `sin(32πf/Fs)/(32 sin(πf/Fs)) ≈ 0.970` at `Fs = 16.368 MHz`. Run `bitplane_test --bench` to compare the rotation with a full-rate scalar reference. [Word correlators](../../receiver/bitplane.cpp)

Set `-DEPHEMERIS_BITPLANE_TRACKING=ON` in a native or firmware CMake build to close the tracking loops with bit-plane population counts. Adding `-DEPHEMERIS_PREFIX_SUMS=ON` selects an equivalent implementation. It shares 32-sample cumulative I/Q sums across channels and reconstructs each signed correlation from up to three contiguous chip runs. Both paths feed the same carrier, code and navigation loops. Native builds default to the sample-based reference. Pico development builds default to the two-core half-chip tracker. In a 65 s synthetic 550 km replay with 128 acquisition steps per millisecond, word tracking reached its first fix at 32.6 s. Position and velocity errors were 1.32 m and 0.046 m/s at the 95th percentile. The population-count and prefix builds produced byte-identical 5 Hz receiver CSVs over the full 65 s replay and a separate 48 s static replay.

At 150 MHz, the RP2350B ran the prefix firmware from SRAM against a local repeated I/Q fixture. Mean processing times were 1,848 µs, 4,609 µs and 8,229 µs per 1 ms of input for one, four and eight channels. In each run all requested channels remained locked for 990 of 990 measured epochs. Isolated prefix preparation took 943–974 µs per epoch, and one tracker took 871–877 µs. The eight-channel computation consumes 8.23 times the available wall time before concurrent acquisition or live capture. [Bench procedure](../../firmware/README.md)

### Half-chip tracking on the Pico

The default Pico kernel integrates eight ADC samples into one complex half-chip sample, then packs each axis sign into 32-bit words. A 128 KiB table holds exact four-sample quarter-IF sums. E/P/L code masks share the same fractional code phase. The correlator handles ordinary orbital Doppler with a quadrature phase lookup and XOR. At low Doppler, it groups despread samples over at most 1/32 carrier turn and rotates each accumulated group at its midpoint. This preserves a continuous prompt phase through zero Doppler. The acquisition core computes alternate channels on a doorbell interrupt. The original capture owner consumes the channel results and updates LNAV and navigation state. [Measured tracking costs](results.md) · [Source](../../receiver/coarse.cpp)

In a release build with Arm GNU 14.2.Rel1, the live image uses 487,624 bytes of the 512 KiB main SRAM region for code, data and static allocations. The first free byte is at `0x200770c8`, leaving 36,664 bytes below `0x20080000`. The linker assigns the core 0 stack separately in the scratch banks. Recheck the `ephemeris_pico.elf.map` file when changing the lookup table or DMA ring size.

## LEO acceptance

The fixed suite uses 400, 550 and 800 km circular receiver orbits at 97.6° inclination, four different start phases per altitude, and a zenith-facing antenna. Each nominal cold start must achieve five successive truth-checked fixes beginning within 120 s. At each altitude, measure 95th-percentile 3D position ≤10 m and velocity ≤0.2 m/s over one complete nominal orbit. At least 99% of eligible epochs after acquisition must have a valid solution. Record a status every 200 ms and require zero sample loss through acquisition and tracking. Eligibility comes from the truth geometry, with six healthy signals at ≥40 dB-Hz and PDOP ≤3. [Frozen cases](../../tests/scenarios.json)

```sh
python3 tools/campaign.py prepare build/current/leo --full-orbits
python3 tests/leo_observables.py build/current/native/geometry_probe --full
python3 tools/run_campaign.py build/current/native/eph-replay build/current/raw --scenario leo-550-1
```

The first command generates host-side synthetic trajectories. The second drives the production navigation solver directly with broadcast orbits, transmit times and Doppler over 12 starts. The third streams simulated RF samples through the complete cold receiver. Use `--scenario all --full-orbits` to run the long raw campaign. Large captures stay outside Git. Keep hashes, the generating command, firmware version, calibration, truth, clock rates and raw telemetry with each hardware result.

For conducted Safran runs, record receiver ECEF truth, healthy SV counts above the signal threshold, PDOP and signal levels. Capture the Pico sample marker relative to simulator time, including fixed RF delay. Calibrate ADC rate as well as offset with two separated timing observations. At 7.5 km/s a 1 ms timing error looks like 7.5 m of position error. Normalize truth to the documented 5 Hz CSV columns and run `tools/campaign.py score TRUTH.csv RECEIVER.csv --sample-zero-s OFFSET --sample-rate-hz HZ`. Use receiver power-on as t=0 for cold-start scoring. Keep RF and navigation hints disconnected from the receiver input except for the simulated antenna feed. [Safran raw logging](https://safran-navigation-timing.com/online-manuals/skydel/Content/SDX/Skydel/Settings-Global.htm)
