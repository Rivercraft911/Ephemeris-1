# Pico Plus 2 firmware

The live image starts the MAX2769B and receiver at power-up. The other images exercise the capture bus and receiver timing. Build for the **Pimoroni Pico Plus 2 (RP2350B)** with Pico SDK 2.2.0, Arm GNU 14.2, CMake, Ninja and picotool. Set `PICO_SDK_PATH` to the SDK checkout. The SDK also accepts `PICO_TOOLCHAIN_PATH` for a non-default compiler installation.

```sh
cmake -S firmware -B build/current/pico -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/current/pico
```

A fresh Pico build uses the half-chip tracker and shares the channels between both cores. The installed live and profile images copy their code into SRAM at boot, matching the execution memory used for timing. Select the scalar reference with `-DEPHEMERIS_COARSE_TRACKING=OFF -DEPHEMERIS_DUAL_TRACKING=OFF` in a separate build directory. The native build defaults to that reference.

Each target produces a UF2 and a matching ELF/map in that directory. Use normal BOOTSEL drag-and-drop on the intended Pico. Connect a serial terminal to the live image for 5 Hz status CSV. USB enumeration and host reads are optional. Send `stats` followed by a newline for DMA loss, FIFO stalls, clock timeouts, maximum backlog, processing time, dropped USB records and watchdog-retention status. Timing histogram bins are 50 µs wide. The final bin includes everything at or above 3,150 µs.

Send `observables` for one snapshot, or `observables on` for a batch with each status update. Send `observables off` to stop. Each active channel emits `#obs,sample,prn,locked,doppler_hz,sv_tx_tow_s`. The sample index is the common block end. The last field stays empty until the decoder has transmit time. Each batch ends with `#obs_end,sample,count`, including zero-channel batches. Backpressure drops superseded rows and increments the USB loss count. Unplugging USB turns off streaming.

To switch images, send the complete line `bootsel` to a CDC-enabled image. The board enters the ROM USB bootloader.

| Image | Use |
| --- | --- |
| `ephemeris_pico` | Autonomous single-chip receiver. The tracking/PVT worker owns capture and USB on core 1 with a dedicated 16 KiB stack. Core 0 runs the interruptible acquisition search. Both stacks have hardware guards. |
| `ephemeris_profile` | Single-Pico 1/4/6/8-channel replay and timing bench. The default command uses an internal PIO sample clock and real DMA traffic. No RF board or wiring is needed. |
| `ephemeris_capture` | Receive a finite 10 ms recording, or simultaneously compare the normal packed input with four one-bit PIO lanes. Use the internal pattern generator, an independently clocked fixture, or the live front end. |
| `ephemeris_pattern` | Independent PRBS data and clock from a **second** Pico. Use only on the transmitting fixture. |

## Wiring the live front end

GPIO numbers below are RP2350B GPIO numbers. Read the pin-1 markings on both boards. Give J3 its regulated 5 V supply, supply J2 pin 16 from Pico 3V3, and share ground. Power the front end before starting the receiver so its oscillator has time to start. J2 is the level-translated host interface. [Rail, clock and host design](../docs/design.md#j2-host-interface)

| J2 | Function | Pico GPIO |
| ---: | --- | ---: |
| 1 | I sign | 5 |
| 3 | I magnitude | 4 |
| 5 | Q sign | 3 |
| 7 | Q magnitude | 2 |
| 9 | Sample clock | 6 |
| 11 | CS_N | 17 |
| 13 | SCLK | 18 |
| 15 | SDATA | 19 |
| 2, 4, 6, 8, 10, 12, 14 | Ground returns | GND |
| 16 | Host VIO | 3V3 |

**GP7** is an output to a scope or independent timing instrument. It rises on the packed lane's first PIO `IN` instruction and stays high through the capture segment. Record this edge together with the simulator time reference and board reset/power-on. The first signal anchors sample time. The second starts the cold-start stopwatch. A new GP7 rising edge during a live run marks a capture restart. No board signal connects to GP7. In the profile image GP8 emits a development clock. Leave GP8 unwired.

The default PIO loop waits for a complete rising transition on GP6 and samples all four data pins on the following state-machine instruction. The GPIO input synchronizers stay enabled. Probe GP6 and the four data pins at J2. If the measured eye favors the opposite edge, rebuild with `-DEPHEMERIS_CAPTURE_FALLING=ON` and record the setting with the capture. The bit mapping on disk is `I1 I0 Q1 Q0`, earliest time in the low nibble.

## Single-Pico timing test

Install `pyserial` and NumPy in the host Python environment. Load `ephemeris_profile.uf2` and run the commands below, substituting the actual CDC device for `PORT`.

```sh
python3 firmware/bench.py PORT profile 1
python3 firmware/bench.py PORT profile 4
python3 firmware/bench.py PORT profile 6
python3 firmware/bench.py PORT profile 8 --reference build/current/native/eph-replay
```

Each command uploads an independently generated, repeating one-millisecond GPS code fixture. The Pico initializes the receiver at sample zero and seeds the requested trackers at known code phase and zero Doppler before arming DMA. The following 999 epochs are paced by real PIO/DMA blocks, beginning at sample 16,368, while acquisition runs on the other core. Loss accounting starts before the first paced block. The report includes channel masks, worst and p95 processing time, lost blocks, and per-channel final prompt and phase. For this controlled fixture, passing requires all requested channels to lock after ten epochs and remain locked, maximum processing time below 1,000 µs, and zero data loss. Eight occupied channels leave no free channel for acquisition handoff. With `--reference` the host replays the same tracking epochs through the native C++ build and checks sample index, lock, code phase (0.05 chip tolerance), Doppler (1 Hz), and complex prompt (5% of reference magnitude). This benchmark measures scheduling and correlation cost. Use navigation-message and orbital replay for navigation accuracy.

By default, the clock is generated on the same chip. To measure an independently clocked DMA workload, supply GP6 from the external pattern fixture and append `--external`. Only that run should be labeled externally clocked. Hardware timing results require a connected Pico.

Use `profile 8 --offline --reference build/current/native/eph-replay` to measure lossless arithmetic on the Pico even when the live deadline fails. Offline mode stops capture DMA, pauses the search core and runs 1,000 consecutive epochs in sample order. Its pass checks lock continuity. The deadline applies to the live modes. The report gives mean and p95 receiver time over the last 990 epochs and worst time over all 999 updates. Offline mode also times input preparation and one active tracker separately, averaging 48 updates after eight warm-up epochs. Commands for 1, 4 and 6 channels use the same syntax. For the experimental closed-loop bitplane path, build firmware and native replay in separate directories with `-DEPHEMERIS_BITPLANE_TRACKING=ON`, then use those artifacts together.

For a two-core tracking comparison, configure a separate firmware build with `-DEPHEMERIS_DUAL_TRACKING=ON` and the same receiver backend. Live and profile images give the odd-numbered tracker slots (indices 1, 3, 5, 7) to core 0. A hardware doorbell interrupts acquisition; both cores read the same prepared epoch and update disjoint trackers. Core 1 joins before decoding or reporting. An eight-channel run therefore measures four trackers per core. The profile and live stats report `tracking_cores`. Offline mode pauses acquisition while keeping the tracking interrupt enabled. Use the same native backend for numerical comparisons.

After a dual-core profile, `core0_stack_untouched_bytes` counts the contiguous marked bytes still untouched above the hardware stack guard. It measures stack use since boot, including interrupts that preempted acquisition. A value of `-1` denotes the serial build.

Run `python3 firmware/bench.py PORT profile 8 --pvt-load` for a combined workload. While tracking all eight replay channels under live DMA, core 1 solves a six-satellite RTKLIB week-rollover fixture at 200, 400, 600, 800 and 1,000 ms. Every solution must have the expected GPS week/TOW, position within 0.20 m and velocity within 0.004 m/s. This bench fixture has independent observables; its solutions do not enter the receiver state. The profile reports separate tracking and fixture-solve times, maximum combined compute time, peak unread bytes and backlog recovery delay. Each burst must drain back below one complete epoch. The final burst gets up to 50 more clocked epochs to recover, with every added epoch counted in tracking, validity and loss checks. Use `--reference` to compare the final tracking epoch to the same native backend.

The profile image also supports `python3 firmware/bench.py PORT continuity --seconds 270`. It consumes the continuously clocked DMA ring and verifies every block index, including the hardware transfer-count reload after about 66 seconds and the 32-bit sample boundary after about 262 seconds. The returned sample count and elapsed microseconds give the average digital input rate against the Pico timer. The generator and timer share a crystal. This run measures capture continuity without tracking or external RF.

### Cold first-channel timing

Start a fresh profile image for each trial. The bench rejects a blind trial if the receiver has already searched or used seeds in that boot.

```sh
python3 firmware/bench.py PORT blind-lock 1 --seconds 60
# Restart the profile image, then run the next trial.
python3 firmware/bench.py PORT blind-lock 4 --seconds 120
```

The host uploads one millisecond of I/Q with the chosen GPS code at 137.25 chips and zero Doppler. The fixture repeats the same noise samples and carries a constant data bit. Internal PIO and DMA pace the receiver at one block per millisecond. The search starts at PRN 1 and receives the waveform alone. The device records the first epoch of a 20 ms interval with one continuously locked PRN. The host checks the PRN, code phase and Doppler. A wrong PRN fails even if the tracking loop declared lock. An earlier compute-paced native build produced a wrong early lock on the PRN 4 fixture. The cross-code regression now exercises that failure with independent noise. This repeating waveform serves as a timing test. RF sensitivity and false-lock rates need independent noise and changing signals.

With the original FFT on the 150 MHz Pico the sampled and wall times were 10.628 s and 10.628185 s for PRN 1, then 42.500 s and 42.499627 s for PRN 4. Both runs recorded zero sample gaps, missed blocks, FIFO stalls and clock timeouts. Each result measures one cold channel acquisition. A position fix also requires decoding satellite clocks and orbits on multiple channels.

## Single-Pico digital capture

Load `ephemeris_capture.uf2` on an unwired Pico. These commands generate a PRBS on an isolated GP8–GP12 bus using PIO2 and read the pad states back through PIO0/PIO1. Leave GP8–GP12 free when using the internal source.

```sh
python3 firmware/bench.py PORT capture build/current/pattern-internal.eiq --internal --prbs
python3 firmware/bench.py PORT compare --internal
python3 firmware/bench.py PORT timing --internal
```

The first command checks the entire decoded pattern and its chronological nibble order. The second compares the four bitplane state machines with the nibble state machine on identical edges. Both paths share the Pico system clock. The independent-clock check below adds off-board timing and wiring. Only the assembled RF test exercises MAX2769B levels, clock/data skew, and mixer response.

`timing` captures both representations simultaneously and runs the experimental early/prompt/late correlator on each. It compares every converted word and all six output components per channel. The reported microseconds are totals over ten 1 ms epochs, with separate input-conversion and 1/4/8-channel correlation times. Divide each total by ten for mean time per epoch. Odd milliseconds start halfway through a bitplane DMA word. This bench times computation after capture. Use `profile` for live receiver deadlines. Omit `--internal` to use the external bus.

Run `python3 firmware/bench.py PORT coarse --internal` to time the experimental one-bit, half-chip correlator on the same type of recording. It times shared preparation and 1/4/8 open-loop early/prompt/late correlations. For each channel count, `worst_margin_us` gives 1,000 µs minus the worst combined preparation and correlation time observed in ten epochs. The remaining work for a complete receiver includes loop updates, acquisition and navigation. The bench checks the parity and range of every sum. Native signal tests check numerical accuracy.

The same run simultaneously captures each GPIO as a bitplane. The Pico computes the sign of each eight-sample sum directly from the sign and magnitude bits, four groups per 32-bit word. The derived I and Q signs must match packed preprocessing for all 20,460 half-chips. `pio_prep_total_us` reports preparation time, and `worst_pio_margin_us` gives the resulting margin for each channel count.

`python3 firmware/bench.py PORT pairs --internal` samples each two-bit ADC axis in its own PIO state machine. Each 16-bit lane field holds eight successive samples. A 128 KiB lookup table gives the alternating sums of even and odd samples. Combining one I lookup and one Q lookup performs the quarter-IF rotation and half-chip integration. The SRAM image reuses the packed snapshot for the table after computing all ten reference epochs. The scored computations exclude one-time table construction.

## Independent pattern and RF capture

Flash `ephemeris_pattern` on the second Pico and `ephemeris_capture` on the receiving Pico. Wire GP2 through GP6 straight across and connect grounds. These are 3.3 V digital signals. Disconnect the GNSS board's data/clock outputs when the pattern Pico drives the same pins.

```sh
python3 firmware/bench.py PORT capture build/current/pattern.eiq --prbs
python3 firmware/bench.py PORT compare
```

The first command finds the unknown PRBS phase and checks **every one of 163,680 received nibbles** against an independent Fibonacci-form reference, including pattern-ring wraps. An aborted or invalid capture preserves the previous output file. The pattern Pico synthesizes a clock near 16.368 MHz with a fractional PIO divider. Measure its actual frequency for timing characterization. The second command starts five state machines in two PIO blocks on the same cycle, then checks all four bitplanes against the ordinary nibble stream. Exercise transitions such as the PRBS pattern; a single logic level provides little coverage.

For real RF, remove the pattern source, connect the powered Ephemeris J2 bus, and start `ephemeris_capture` again. Check TP1 and the clock at J2. Capture the known L1 tone with `python3 firmware/bench.py PORT capture build/current/tone.eiq`. The response contains a 32-byte EIQ1 header and 81,840 packed bytes ready for native replay. Check the measured IF, I/Q sign and pin order. [RF bring-up](../docs/design.md#first-capture)

## Capture integrity and restarts

The continuous DMA channel writes 32-bit words into a 32 KiB hardware-wrapped SRAM ring. Its self-reloading transfer count spans a ring boundary without an IRQ handoff. The consumer copies 8,184-byte epochs and rechecks the completed-write cursor after the copy. An overwritten or torn epoch is dropped, counted and signaled to the receiver. A sticky RX FIFO stall or prolonged clock loss starts a new segment and invalidates tracking. The sample index is monotonic across segments; missing clock edges across a stall have unknown absolute time. Re-anchor using the marker before scoring post-fault timing. TP1 is a physical test point and is not wired to a Pico status input.

A second PIO state machine latches a stopped-high or stopped-low clock, including a pause that ends before the CPU next polls. At 150 MHz the threshold is about 1 µs on either level. Shorter disturbances require checking the source waveform or the independent PRBS reference. Exercise each clock level during bench fault injection and check `clock_timeouts`, the next GP7 segment edge and the invalid status.

On the single Pico, run `python3 firmware/bench.py PORT clocktest` with the profile image. It holds the generated clock low, then high, for 10 µs each. Both times the source resumes before the CPU reads the DMA cursor. The check requires a new gap-marked epoch, monotonic sample indices and exactly one recorded fault for each pulse.

On the live image `warmboot` saves tracked PRNs, carrier estimates and available ephemerides to checksummed uninitialized SRAM before an immediate watchdog reboot. Only the explicit reset cookie and a watchdog reset permit loading that snapshot. `coldboot` clears that cookie and restarts without the hint. Power cycles, normal pin resets and bad checksums start cold. Retention provides no decoded time or previous valid fix. Physical power-cycle tests remain the cold-start qualification procedure.

### Cold I/Q replay on the Pico

The `ephemeris_replay_bench` image receives an EIQ1 recording over USB CDC. The M33 starts with empty channels and no time, PRN, Doppler, or ephemeris hints. Core 0 validates each four-millisecond frame's sample interval, sequence number and CRC32. Core 1 processes the raw samples, performs a bounded number of acquisition steps after each millisecond, decodes LNAV, and emits the receiver's 5 Hz status. Two frames may be in flight. Their acknowledgements include separate tracking and acquisition times.

The prefix-sum build used for the bench can be reproduced with the Pico SDK and the standard board toolchain. Set `PICO_SDK_PATH` first.

```sh
cmake -S firmware -B build/pico-replay -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPICO_NO_FLASH=1 -DEPHEMERIS_BITPLANE_TRACKING=ON -DEPHEMERIS_PREFIX_SUMS=ON
cmake --build build/pico-replay --target ephemeris_replay_bench
picotool load -x build/pico-replay/ephemeris_replay_bench.elf
python firmware/replay_bench.py PORT leo.eiq receiver.csv --work 2 \
  --stop-after-fixes 5 --metrics transport.json --truth truth.csv --sample-zero-s 0
```

The host needs pyserial and Python 3.10+. Truth scoring also uses NumPy. The last two options above use the synthetic fixture's known alignment. The host opens the independent truth file after the USB session closes. It scores the 5 Hz interval actually processed and records the result with transport metrics. Omit `--stop-after-fixes` for the full recording, or use `--max-ms 250` for a transport check. The final CSV contains only statuses reported by the target. An interrupted transfer leaves a `.partial` file with the received rows. Close the serial port to abandon a session. A new `replay` command starts cold, and `bootsel` at the command prompt returns to the boot ROM.

Playback follows the host's backpressure. Recorded time sets the simulated receiver dynamics. The metrics JSON reports input bytes and seconds of recording per wall second. It records first and last valid host arrival offsets on a monotonic clock. During the longest unbroken run of valid reports, the measured wall-clock fix rate is `(reports − 1) / (last arrival − first arrival)`. The recording's scheduled 5 Hz cadence is recorded separately. A 250 ms stock-clock SRAM smoke run with two acquisition steps per millisecond processed 497 search steps in 4.64 s wall time. The prefix-sum image processed 45.404 s of a synthetic 550 km LEO recording in 926.77 s of wall time. It cold-acquired seven satellites and delivered five valid fixes from recorded times 44.6 through 45.4 s. Their four arrival intervals spanned 20.717 s on the host clock, giving 0.193 Hz, or one fix every 5.18 wall seconds. The independent truth score over those five fixes measured 1.02 m position and 0.0366 m/s velocity error at the 95th percentile. This mode supports end-to-end testing before RF assembly. The live capture and conducted RF procedures measure timing at the hardware boundary.


### Clocked I/Q replay on the Pico

The experimental `ephemeris_clocked_replay` bench runs acquisition, tracking, navigation decoding and the solver on the Pico under a 1 kHz device clock. To fit USB full-speed, the host mixes each block to baseband, integrates eight ADC samples and retains the I and Q signs per half-chip. Each millisecond occupies 512 bytes. The Pico reconstructs a unit-amplitude IF waveform for the normal receiver. The recording supplies neither satellite identities nor navigation hints. The live image still takes full-rate ADC samples from PIO.

```sh
cmake -S firmware -B build/pico-clocked -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPICO_NO_FLASH=1 -DEPHEMERIS_GROUND_SEARCH=ON
cmake --build build/pico-clocked --target ephemeris_clocked_replay
python3 firmware/clocked_replay.py pack ground.eiq ground.bin
picotool load -x build/pico-clocked/ephemeris_clocked_replay.elf
python3 firmware/clocked_replay.py run PORT ground.bin clocked.csv
python3 tests/independent_static.py clocked.csv --seconds 165
```

The scorer above expects the independent 165-second static fixture. Use the corresponding orbital scorer and a full-range build for the orbital recording. For a matched native input, run `python3 firmware/clocked_replay.py expand ground.bin rebuilt.eiq`. Packing requires a recording with sample origin zero. Reload the SRAM bench between cold trials.

The device clock starts after a 64 ms input prefill. Host timing starts at the USB handshake. A complete transport pass requires the expected CRC, all scheduled 5 Hz statuses, zero input underruns and less than 4 ms of maximum processing lateness. The recording's truth scorer checks position, velocity and GPS time separately. Incomplete runs leave a partial CSV.

In the first two hardware trials the input queue ran dry at 2.796 s and 13.362 s. The second used a 512-byte USB endpoint transfer. Peak processing lateness was 1.19 ms in that trial. Both runs ended before a position fix. The current build gives USB service priority during tracking. [Timing and validation results](../docs/software/measurements/pico-acquisition.json)
