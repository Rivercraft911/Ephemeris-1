# A cold fix from recorded sky

CTTC recorded 100 seconds of GPS L1 in Castelldefels, Spain, on April 4, 2013. The original front end sampled complex baseband at 4 MS/s and saved signed 16-bit I and Q. For this replay the receiver starts with empty tracking channels and navigation state. Its only input is the waveform. [GNSS-SDR recording and example fix](https://gnss-sdr.org/my-first-fix/)

The 150 MHz Pico Plus 2 returned its first position at **41.274846° N, 1.987684° E, 81.69 m WGS 84 ellipsoid height**. The laptop received it 55.24 s after the USB handshake. The Pico used five satellites for that fix and decoded PRNs 1, 11, 17, 20, 23 and 32 during the recording.

| Pico measurement | Result |
| --- | ---: |
| First satellite lock / four locks, laptop clock | 1.44 s / 26.64 s |
| Consecutive valid fixes | 225 over 44.80 s from first to last |
| Measured fix rate | 5.0001 Hz |
| Sample gaps / input underruns | 0 / 0 |
| Apparent speed of the stationary antenna, p95 | 1.53 m/s |

The native receiver reached a six-satellite fix at 25.2 s of recording time with eight search steps per millisecond. A second native run used the same compact input as the Pico and a search budget calibrated on the target. Its first position came at 55.2 s. [Run settings, input hashes and scores](measurements/cttc/report.json)

GNSS-SDR 0.0.19 independently processed the original 4 MS/s recording. Of the Pico's 225 fixes, 223 fell within the reference's 10 Hz time series. Median separation was **2.56 m horizontally** and **6.11 m in 3D**. At the 95th percentile it was **5.80 m horizontally**, **13.75 m vertically** and **13.94 m in 3D**. The height difference accounts for most of the larger excursions in the plot.

![Horizontal, vertical and 3D separation between simultaneous Pico and GNSS-SDR fixes](measurements/cttc/separation.png)

The capture notes identify a NovAtel GPS-600 on the CTTC roof. Their approximate coordinates disagree with the archived GNSS-SDR track and both fresh solutions. We have no surveyed antenna position for this recording, so these figures measure agreement between receivers. [CTTC site](https://www.cttc.cat/contact/) · [Original recording](https://gnss-sdr.org/my-first-fix/)

## Reproduce the replay

Download the archive from the publisher and check its SHA-256 before unpacking. [Original CTTC archive and checksum](https://sourceforge.net/projects/gnss-sdr/files/data/2013_04_04_GNSS_SIGNAL_at_CTTC_SPAIN.tar.gz/download)

```sh
mkdir -p build/cttc
cd build/cttc
name=2013_04_04_GNSS_SIGNAL_at_CTTC_SPAIN
curl -fL "https://downloads.sourceforge.net/project/gnss-sdr/data/$name.tar.gz" -o source.tar.gz
echo "d5b926aefe7462ca4211bcae2129591a810fa4960a214f35d056a883aa2af3ff  source.tar.gz" | shasum -a 256 -c -
tar -xzf source.tar.gz "$name/$name.dat"
cd ../..
python3 tools/import_cttc.py "build/cttc/$name/$name.dat" build/cttc/cttc.eiq
python3 firmware/clocked_replay.py pack build/cttc/cttc.eiq build/cttc/sky.bin
```

The adapter resamples by 1023/250, shifts baseband to +4.092 MHz, then quantizes each axis to the board's two-bit format. Its fixed magnitude threshold is the per-axis RMS over the first 100 ms, 122.59 input counts. For USB full-speed the host packs each half-chip into two sign bits. Acquisition, tracking, navigation-message decoding and the position solve all run on the Pico.

```sh
cmake -S firmware -B build/cttc/pico -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPICO_NO_FLASH=1 -DEPHEMERIS_GROUND_SEARCH=ON -DEPHEMERIS_GROUND_TROPOSPHERE=ON \
  -DEPHEMERIS_DOPPLER_POSITION=ON -DEPHEMERIS_VELOCITY_FIT=ON
cmake --build build/cttc/pico --target ephemeris_clocked_replay
picotool load -x build/cttc/pico/ephemeris_clocked_replay.uf2
python3 firmware/clocked_replay.py run PORT build/cttc/sky.bin build/cttc/pico.csv
```

Score the archived Pico fixes against the independent receiver, or substitute the CSV from a new replay. [Reference receiver configuration](measurements/cttc/reference.conf) · [Pico bench setup](../../firmware/README.md#clocked-iq-replay-on-the-pico)

```sh
data=docs/software/measurements/cttc
python3 tools/compare_ground.py "$data/pico.csv.gz" "$data/gnss-sdr.csv.gz" --plot build/cttc/separation.png
```

GNSS-SDR used the archive date to select GPS week 1734. Ephemeris decoded week 710 modulo 1024. For the comparison, each published GNSS-SDR GPX epoch was matched to its double-precision ECEF position in the RTKLIB binary log. We subtracted the fitted receiver clock offset from its receive time, then interpolated the reference position at each Pico fix time. The scorer excludes times outside the reference and gaps longer than 0.15 s.

To regenerate the reference with GNSS-SDR 0.0.19, use a fresh output directory so the GPX belongs to this run.

```sh
mkdir -p build/cttc/reference
cd build/cttc/reference
gnss-sdr --config_file=../../../docs/software/measurements/cttc/reference.conf
python3 ../../../tools/cttc_reference.py pvt.dat pvt_*.gpx > reference.csv
```
