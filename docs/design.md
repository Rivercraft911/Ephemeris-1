# Ephemeris 1 Rev 1.0 design and bring-up

The first PCBs arrived November 18, 2026. Assembly, first power and sample capture are pending. [Interactive assembly BOM](assembly/ibom.html) · [Parts and substitutions](ordering-bom.md)

## Circuit

- J1 is a Samtec SMA-J-P-H-ST-EM3 drop-in SMA jack, mounted in a routed edge notch with top-side solder lands. C1 blocks DC into LNA1. C2 AC-couples LNAOUT to MIXIN using the MAX2769B datasheet's direct path. Select LNA1 explicitly in firmware and leave antenna bias disabled. The first spin omits the interstage SAW filter.
- Y1 is a 16.368 MHz clipped-sine TCXO on the receiver supply, AC-coupled to XTAL. The PLL filter uses 27 pF in parallel with a series 20 kΩ / 470 pF branch. The values follow the datasheet starting point for a 1.023 MHz comparison frequency and 0.5 mA charge pump.
- J3 takes regulated 5 V on pin 1 and ground on pin 2. U2 produces 3.0 V, with a static tolerance range of 2.955–3.045 V at ±1.5%. The MAX2769B operating range is 2.7–3.3 V. TP2 and TP3 are the receiver rail and ground. TP1 exposes the PLL lock indication.
- The MAX2769B is the T2855+3 package, 5 × 5 mm, 0.5 mm pitch and nominal 3.25 mm exposed pad. U1 pad 29 maps to the exposed ground paddle. The project-local footprint follows ADI land drawing 90-0023 and uses four exposed-pad paste windows.
- U3 translates the four CMOS sample bits to host VIO. U4 translates the three configuration lines toward the receiver and the reference clock toward the host. Use 3.3 V host VIO for initial bring-up. Verify a different host's I/O voltage, thresholds and timing before connecting it. R2–R6 are initial source damping values.

## Passive parts

| References | Qty | Value and package | Manufacturer part number | Function |
| --- | ---: | --- | --- | --- |
| C1, C2, C6 | 3 | 470 pF, C0G, 50 V, 0402 | Murata `GRM1555C1H471JA01D` | RF series DC blocks and PLL filter |
| C3 | 1 | 10 nF, X7R, 50 V, 0402 | Murata `GRM155R71H103KA88D` | TCXO AC coupling |
| C5 | 1 | 27 pF, C0G, 50 V, 0402 | Murata `GRM1555C1H270JA01D` | PLL shunt cap |
| C8 | 1 | 100 pF, C0G, 50 V, 0402 | Murata `GRM1555C1H101JA01D` | VCC_RF high-frequency bypass |
| C7, C9–C13 | 6 | 100 nF, X7R, 16 V, 0402 | Murata `GRM155R71C104KA88D` | U1 supply bypass |
| C4, C14–C17 | 5 | 100 nF, X7R, 16 V, 0603 | Murata `GRM188R71C104KA01D` | TCXO and translator supply bypass |
| C18, C19 | 2 | 4.7 µF, X7R, 25 V, 0805 | TDK `C2012X7R1E475K125AB` | 5 V input and 3.0 V LDO output |
| R1 | 1 | 20 kΩ, 1%, 0603 | Yageo `RC0603FR-0720KL` | PLL loop filter |
| R2–R6 | 5 | 22 Ω, 1%, 0603 | Yageo `RC0603FR-0722RL` | Initial source damping |
| R7–R10 | 4 | 47 kΩ, 1%, 0603 | Yageo `RC0603FR-0747KL` | Configuration line defaults |

The 0402 footprints keep the RF, reference and QFN bypass paths short. For U1, place C7/C8 at pin 4, C9 at pin 11, C10 at pin 13, C11 at pin 14, C12 at pin 19, and C13 at pin 23. Route the 3.0 V branches from the regulator output region with a nearby ground return at each pin. The 470 pF RF couplers follow ADI's typical application. The exact selected MLCC’s GHz impedance has not been established here; verify the receiver RF response during prototype characterization. [ADI typical application](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf) · [Murata 470 pF model](https://ds.murata.com/simsurfing/mlcc.html?partnumbers=%5B%22GRM1555C1H471JA01%22%5D)

## Rail budget and noise

Use a regulated 5 V input. The budget assumes 4.75–5.25 V at J3 and 3.3 V host-supplied VIO at J2. U2 regulates only the receiver rail. LDO tolerance including line, load and temperature gives 2.955–3.045 V. That leaves 255 mV of static margin to either MAX2769B limit and fits the selected TCXO's supply range. At 4.75 V input the LDO is 1.45 V above TI's 3.30 V minimum input for the stated accuracy. [TPS7A20](https://www.ti.com/lit/ds/symlink/tps7a20.pdf) · [MAX2769B](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf) · [Taitien TX](https://www.taitien.com/wp-content/uploads/2015/12/XO-0076_TX.pdf)

| Load | Estimate or design allowance |
| --- | --- |
| U1 on RX_3V0 | 40 mA allowance. ADI specifies 31 mA max for its default LNA1 mode with only I output. Check actual I/Q mode current on the bench. |
| Y1 on RX_3V0 | 1.5 mA max at 16.368 MHz per the TX series sheet. |
| Translators and configuration pulls on RX_3V0 | About 1 mA by the switching estimate below. Allocate 3 mA for this group. |
| RX_3V0 total | Budget 50 mA, including margin. At 5.25 V in and 2.955 V out, U2 dissipates about 115 mW. TI's DBV reference-board 187.1 °C/W gives an estimated 21 °C rise. |
| HOST_VIO | Budget 15 mA from the host. About 9 mA by the switching estimate at 3.3 V, plus margin. |

Switching estimate uses `I = C × V × f`, TI's typical 2 pF input-side and 13 pF output-side dynamic capacitance per channel, and an assumed 20 pF external load per output. For the four I/Q outputs and clock, budget one rising transition per line every 16.368 MHz clock period. For three SPI lines, budget one per 1 MHz period. Thus the 3.3 V host-side output term is `5 × 33 pF × 3.3 V × 16.368 MHz ≈ 8.9 mA`. Receiver-side translator switching is about 0.79 mA. Three 47 kΩ pulls add at most about 0.19 mA to RX_3V0, and the host pull adds up to 0.07 mA to HOST_VIO. Cable and host input capacitance change the budget. [TI TXU0104](https://www.ti.com/lit/ds/symlink/txu0104.pdf) · [TI TXU0304](https://www.ti.com/lit/ds/symlink/txu0304.pdf)

C18 and C19 are 4.7 µF X7R. From TDK's typical DC-bias curve, allow 20% loss at 5.25 V and 10% at 3.045 V. Applying −10% initial tolerance and −15% temperature derating gives estimates of 2.88 µF for C18 and 3.24 µF for C19 before aging. Both clear TI's 1 µF design target. TI separately lists a 0.47 µF effective stability floor for C19. [TDK characterization](https://product.tdk.com/system/files/dam/doc/product/capacitor/ceramic/mlcc/charasheet/c2012x7r1e475k125ab.pdf) · [TI recommended operating conditions](https://www.ti.com/lit/ds/symlink/tps7a20.pdf#page=5)

For a conservative narrow supply section, `R = ρL/(wt)` with copper ρ = 1.724 × 10⁻⁸ Ω·m, L = 10 mm, w = 0.15 mm and t = 35 µm gives **32.8 mΩ**. At the full 50 mA receiver budget that drops **1.64 mV**, leaving about 253 mV of lower-rail margin. At 85 °C, using copper’s 0.00393/°C temperature coefficient from 20 °C, the estimate is 41.2 mΩ and 2.06 mV. At 15 mA the same section drops 0.49 mV at 20 °C. A 10 mm-long, 1 mm-wide pour section is only 4.93 mΩ, or 0.25 mV at 50 mA; the actual pours widen beyond their short pad necks. These are DC conductor estimates, excluding contact resistance and transient inductance. They establish rail-drop margin, not a measured noise result.

TI characterizes 7–10 µV RMS of intrinsic LDO noise over 10 Hz–100 kHz at its stated test points. Its typical 20 mA PSRR at 1 kHz, 100 kHz and 1 MHz is 95, 75 and 45 dB. For a 10 mV RMS input tone at each frequency, `Vout = Vin × 10^(−PSRR/20)` gives 0.18, 1.8 and 56 µV RMS respectively. This is an input-ripple estimate at TI's test points. Fast switching noise generated downstream depends on the routing and return paths. [TI electrical characteristics](https://www.ti.com/lit/ds/symlink/tps7a20.pdf#page=7)

L2 and L3 are GND planes; only L2 has local relief under the SMA signal land. L3 remains continuous beneath the routed signals. A compact F.Cu 5 V entry pour joins J3, C18 and U2. Bottom-layer RX_3V0 and HOST_VIO pours distribute power; short branches feed the receiver, TCXO and translators. C18 and C19 sit at U2, with ground vias beside the per-pin bypass capacitors. Keep the host clock and sample traces away from the PLL filter and TCXO. Measure RX_3V0 at TP2 relative to TP3 and near C7/C8 with a short probe ground, first idle and then during continuous I/Q capture. Compare the ripple and 16.368 MHz harmonics between the two states.

## Footprints and hot-plate assembly

- **U1** uses ADI drawing 90-0023 Rev C for T2855+3. The 28 perimeter lands are 0.95 × 0.30 mm on 0.50 mm pitch with opposing rows 4.68 mm apart. The 3.25 × 3.25 mm ground paddle is pad 29 with 0.24 mm nominal clearance to the perimeter lands. [MAX2769B datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf) · [ADI package outline](https://mds.analog.com/api/public/content/tqfn_21-0140.pdf) · [ADI land pattern](https://mds.analog.com/api/public/content/90-0023.pdf)
- **U2–U4 and Y1** — TPS7A20 DBV pin functions are IN 1, GND 2, EN 3, NC 4 and OUT 5. The TXU0104 and TXU0304 use TI's PW TSSOP-14 pin maps, including their different channel-4 directions. The Taitien TX footprint uses 1.00 × 0.90 mm pads on 2.40 × 1.80 mm centers. Its pin 1 is lower left in the top view, with GND on 2, OUT on 3 and VDD on 4. [TI regulator](https://www.ti.com/lit/ds/symlink/tps7a20.pdf) · [TI TXU0104](https://www.ti.com/lit/ds/symlink/txu0104.pdf) · [TI TXU0304](https://www.ti.com/lit/ds/symlink/txu0304.pdf) · [Taitien drawing](https://www.taitien.com/wp-content/uploads/2015/12/XO-0076_TX.pdf)
- **Stencil and ordinary vias** — Start with 0.10 mm laser-cut stainless and Type 3 or finer no-clean paste. U1 retains four rounded 1.25 mm square paste openings at ±0.925 mm centers, 0.25 mm corner radius and about 57.14% paddle coverage, and five 0.60/0.30 mm plated ground vias. **No via filling, plugging or copper capping is specified.** The existing bottom solder mask covers the exposed-pad vias; the front remains open through the paddle's mask opening. This is ordinary mask tenting, not a flat copper cap or guaranteed sealed hole. The divided paste windows leave 0.15 mm nominal clearance to the four off-center holes, improved from 0.005 mm before removing the cap requirement; the central hole has 0.378 mm clearance. These are nominal geometric clearances, not a measured printing-yield result. Keep the controlled stencil deposit; excess paste beneath U1 can lift the package or bridge its perimeter pins. Check the initial assembly for solder loss/voiding and use the paste vendor's reflow profile, measured at the PCB with a thermocouple. Other SMT solder openings are clear of drilled holes after the user's cleanup. The eight SMA ground-land vias remain open on top and are hand-soldered with J1 after SMT reflow. The earlier requirement for nine filled/capped vias was overly restrictive and is superseded. [ADI QFN stencil guidance](https://www.analog.com/en/resources/app-notes/smt-assembly-and-pcb-design-guidelines-for-maxims-standard-wirebonded-quad-flatpack.html) · [ADI thermal-via mask options](https://www.analog.com/en/resources/app-notes/an-772.html) · [JLC via-covering processes](https://jlcpcb.com/help/article/pcb-via-covering)
- **Connectors** — Hand-solder J1, J2 and J3 after SMT reflow. J1 uses the local Samtec notch footprint with no paste apertures. J2 and J3 retain their 2.54 mm pitch and Würth nominal 1.10 mm finished holes. Verify the fab’s finished-hole tolerance. [Samtec SMA](https://suddendocs.samtec.com/prints/sma-j-p-x-st-em3-mkt.pdf) · [Würth J2](https://www.we-online.com/components/products/datasheet/61301621121.pdf) · [Würth J3](https://www.we-online.com/components/products/datasheet/61300211121.pdf)

## J2 host interface

Pin numbers follow the schematic's 2×8 odd/even header. Odd-numbered pins face the host signals. The opposite pins provide ground returns except for the host's VIO supply on pin 16.

| Pin | Signal | At the host | Opposite pin |
| --- | --- | --- | --- |
| 1 | HOST_I1 | I sign input | 2 GND |
| 3 | HOST_I0 | I magnitude input | 4 GND |
| 5 | HOST_Q1 | Q sign input | 6 GND |
| 7 | HOST_Q0 | Q magnitude input | 8 GND |
| 9 | HOST_CLK | Clock input | 10 GND |
| 11 | HOST_CS_N | Chip select output | 12 GND |
| 13 | HOST_SCLK | Serial clock output | 14 GND |
| 15 | HOST_SDATA | Serial data output | 16 HOST_VIO, from host |

## First capture

1. Apply current-limited 5 V and verify the 3.0 V rail. Supply 3.3 V host VIO and use a common ground. Allow at least 2 ms for Y1 to start after the rail is valid; use a 5 ms firmware wait. Bring up with short header wiring. TP2 is a measurement point, not an alternate supply input: do not externally drive RX_3V0 with VIN off because U2 lacks reverse-current protection.
2. Write all ten MAX2769B registers after power-up. Use a 32-bit MSB-first transaction with CS low, sending 28 data bits followed by the four address bits. The device samples SDATA on rising SCLK and latches on rising CS. Start with a slow SCLK such as 1 MHz and hold CS high for at least a full clock period between frames. Then check TP1 for PLL lock and measure the expected 16.368 MHz on HOST_CLK at J2 pin 9.
3. Use CONF2.IQEN and enable both PGAs with CONF3[13:12] = 11. Keep the interface in parallel CMOS mode. CONF3 bit 12 is identified by ADI's clarification. Datasheet Table 9 labels it reserved.
4. Feed a known L1 test signal at a suitable receiver input level. Verify the I and Q lines toggle, then inspect clock and data timing at J2. Choose the capture edge and PIO timing from the measured data-valid window because clock and data take different translator paths.
5. Put I1, I0, Q1 and Q0 on consecutive RP2350B GPIOs in the same PIO window. Use `IN PINS, 4` with autopush after 32 bits and DMA into RAM. Start with a bounded capture and check sample alignment, spectrum and clipping. 16.368 MS/s × 4 bits = 65.472 Mb/s, or 8.184 MB/s packed. A 256 KiB buffer holds about 32 ms. Continuous delivery depends on the host's output transport and buffering.

The exact Y1 `TXETCDSANF-16.368000` is rated −30 to +85 °C; do not claim −40 °C operation for the whole assembly. [Taitien ordering guide](https://www.taitien.com/wp-content/uploads/2015/12/Model-Numbering-Guide_TCXO-Rev307-2015.pdf)

CLKOUT requires particular attention: the MAX2769B limits of VOH ≥ 2.0 V and VOL ≤ 0.8 V leave only 80/90 mV high/low margin against TXU0304 threshold limits at exactly 3.0 V. The TI tables do not explicitly close this margin across the full 2.955–3.045 V rail range. Verify the signal at U4 pin 10 and the J2 capture window on the prototype; a full-temperature interface guarantee has not been established. [ADI clock limits](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf#page=4) · [TI input thresholds](https://www.ti.com/lit/ds/symlink/txu0304.pdf#page=7)

### What should a capture contain?

At the nominal 16.368 MHz sample clock, each rising edge marks one complex sample period. Two I bits plus two Q bits occupy four bits per sample. A 10 ms capture should therefore contain 163,680 complex samples, or **81,840 bytes** when packed two samples per byte. Compare the DMA byte count with the duration measured at J2. A factor-of-two error is a reason to check nibble packing and clock-edge selection before working on acquisition.

For an RF check, feed a conducted tone at 1575.420 MHz through J1 at a level within the receiver's linear range. The programmed LO is 1571.328 MHz. Their difference is **4.092 MHz**, one quarter of the nominal sample rate. Look for a spectral peak at that offset from DC in the complex capture. Its sign depends on I/Q order and sign convention. Record the source level, observed frequency and signs with the capture. A known-tone capture is the first bench milestone. GPS acquisition and a Safran hardware-in-the-loop run come later.

### Register starting point

These 28-bit values were decoded and rechecked against MAX2769B Tables 6–16 and ADI’s Q-channel clarification in the 2026-11-06 implementation audit. They require bench verification. Pack a transaction as `(data << 4) | address`. The sign/magnitude mapping is in Table 2.

| Address | Register | Data | Setting |
| --- | --- | --- | --- |
| 0 | CONF1 | `0xA295163` | Force LNA1, antenna bias off, FCEN 11 for nominal 4.092 MHz, 2.5 MHz IF bandwidth |
| 1 | CONF2 | `0x855028C` | IQEN, two bits per channel, sign/magnitude, CMOS |
| 2 | CONF3 | `0xEAFF1DC` | Enable I and Q PGAs, leave DSP serial streaming disabled |
| 3 | PLLCONF | `0x9EC0008` | Integer N, reference clock output at 1×, 0.5 mA charge pump |
| 4 | DIV | `0x0C00080` | N = 1536, R = 16 |
| 5 | FDIV | `0x8000070` | Preserve vendor default in integer N mode |
| 6 | STRM | `0x8000000` | Preserve vendor default |
| 7 | CLK | `0x10061B2` | Reference-derived ADC clock, fractional divider bypassed |
| 8 | TEST1 | `0x1E0F401` | Preserve vendor default |
| 9 | TEST2 | `0x28C0402` | Preserve vendor default, FCENMSB = 0 |

Nominal PFD = 16.368 MHz ÷ 16 = 1.023 MHz. LO = 1536 × 1.023 MHz = 1571.328 MHz. GPS L1 at 1575.420 MHz lands at 4.092 MHz IF.

## PCB layout

The 45 × 34 mm outline has 3 mm corner radii and runs from (75, 61) to (120, 95) in KiCad. The grid origin is at the upper left and the drill/place origin marker is at the lower left. J1, U1 and the center of J2 share the board centerline at Y = 78 mm. U3 and U4 share X = 103.95 mm and sit 6.5 mm above and below that line.

Use JLCPCB's **JLC04161H-3313**, nominal 1.60 mm, four-layer FR-4 impedance stack, with 1 oz outer and 0.5 oz inner copper. Use ENIG surface finish. Confirm this stack and controlled-impedance processing in the fabrication quote. [JLCPCB stack calculator](https://jlcpcb.com/pcb-impedance-calculator/) · [JLCPCB calculator inputs](https://jlcpcb.com/help/article/user-guide-to-the-jlcpcb-impedance-calculator)

| Layer or material | Thickness (mm) | Layout use |
| --- | ---: | --- |
| L1 / F.Cu | 0.035 | RF, parts and local signal routing |
| 3313 prepreg | 0.0994 | L1 to L2, design Dk ≈ 4.1 |
| L2 / In1.Cu | 0.0152 | GND reference; local SMA-pad relief only |
| FR-4 core | 1.265 | L2 to L3, design Dk ≈ 4.42 |
| L3 / In2.Cu | 0.0152 | GND reference |
| 3313 prepreg | 0.0994 | L3 to L4 |
| L4 / B.Cu | 0.035 | Signals, GND and local RX_3V0 / HOST_VIO pours |

KiCad retains 0.035 mm nominal external copper and 0.01524 mm solder mask over copper on each face, giving a modeled stack of about 1.595 mm and a nominal board thickness of 1.60 mm. The RF calculation uses JLCPCB’s **0.04064 mm finished plated external copper**. Both surfaces remain 0.0994 mm from their adjacent ground plane, Dk = 4.1, so the 3313 stack preserves the existing uniform RF geometry. The Samtec mounts on top-side ledges and has no underside board-clamping tabs. Its body extends 3.02 mm below the PCB top, approximately 1.42 mm below a nominal 1.6 mm board. [Samtec drawing](https://suddendocs.samtec.com/prints/sma-j-p-x-st-em3-mkt.pdf) · [JLCPCB tolerances](https://jlcpcb.com/capabilities/pcb-capabilities)

- **RF routing.** The `SE_50` nets use **0.15 mm F.Cu traces and a 0.20 mm copper-edge gap to top ground**, over L2. JLCPCB’s live coated-coplanar solver returned **49.77 Ω** on 2026-11-06 using h = 0.0994 mm, Dk = 4.1, finished copper = 0.04064 mm, mask Dk = 3.8, mask thicknesses 0.0254 mm over laminate / 0.01524 mm over copper, and a 0.0127 mm reduction from trace base to top width. These current calculator settings differ slightly from its guide’s reference defaults. The result describes the uniform line; solder pads, components and the connector are separate discontinuities. Top ground is stitched to the internal planes along the RF path. [JLCPCB calculator](https://jlcpcb.com/pcb-impedance-calculator/) · [Calculator assumptions](https://jlcpcb.com/help/article/user-guide-to-the-jlcpcb-impedance-calculator)
- **SMA plane relief.** L2 has a local **2.50 × 1.50 mm** void from x = 82.0–84.5 mm, y = 77.25–78.75 mm beneath the broad signal land. L3 remains ground. The 0.15 mm feed returns to its close L2 reference immediately after the land and retains a 0.20 mm top-ground gap. A simple parallel-plate estimate for the 2.19 × 1.00 mm land over 0.0994 mm FR-4 gives about 0.80 pF before fringing, motivating local L2 relief. This is a reasoned connector transition, not a measured or full-3D-verified 50 Ω launch. Check the assembled launch at GPS L1 during prototype characterization.
- **Routing rules.** Signal tracks are at least 0.15 mm wide with 0.20 mm minimum copper clearance. All 186 vias have 0.30 mm finished holes: 35 have 0.50 mm copper lands and 151 have 0.60 mm lands. The minimum annular ring is 0.10 mm. None require filled/capped processing. Preserve the hole/clearance limits and verify filled copper after any edit.
- **Return paths and stitching.** Surface signals reference the adjacent ground plane across 0.0994 mm of prepreg. The intentional L2 relief is confined to the SMA launch. All nine remaining signal-layer transitions have nearby ground vias; maximum center distance is 1.693 mm at the Q0 escape. The receiver clock escape is 1.359 mm from its nearest ground via. All 159 ground vias connect the four ground-bearing copper layers. The nominal 2 mm perimeter/launch fence pitch is about one fifty-fourth of the guided wavelength at GPS L1; fast digital edges also govern return-path needs.
- **Fabrication rules and finish.** The project includes 0.15/0.20/0.30/0.50 mm routing presets and usable 0.50/0.30 and 0.60/0.30 mm via presets. Its unused legacy 0.45/0.25 mm preset remains saved, but is below the current 0.30 mm minimum-hole rule and must not be used. Board minimum copper clearance is 0.20 mm; PTH-specific rules enforce 0.30 mm hole-to-copper and 0.45 mm hole-to-hole clearance. Soldermask expansion is +0.025 mm with 0.13 mm minimum web, and silkscreen graphic strokes and mask clearance are 0.15 mm. The former 0.20 mm exposed ENIG border is removed on both faces. Ground copper and the complete perimeter/launch via fence remain under solder mask; functional connector and test-point openings are unchanged. Header pads use thermal spokes for hand soldering; RF ground and QFN paddle connections remain solid. [JLCPCB capabilities](https://jlcpcb.com/capabilities/Capabilities)
- **J1 seating.** The connector notch starts at (75.00, 78.00) mm, preserving the 45 × 34 mm board envelope. Its straight rear wall is at x = 82.00 mm, width **7.12 ±0.10 mm**, and depth **7.00 ±0.10 mm**. Two R0.40 reliefs are centered at (81.80, 74.69) and (81.80, 81.31) mm. The local footprint derives from Samtec’s recommended pattern, with the signal land shortened to 2.19 × 1.00 mm and the ground lands trimmed around the reliefs to retain at least 0.30 mm nominal copper-to-edge clearance. No launch clearance-rule exemption is used. Hand-solder J1 after SMT reflow and any depanelization. [Samtec drawing](https://suddendocs.samtec.com/prints/sma-j-p-x-st-em3-mkt.pdf)
- **Notch fabrication.** JLC’s regular ±0.20 mm routing does not meet this connector’s ±0.10 mm notch requirement. Specify the finished notch contour and preserve both R0.40 reliefs in CAM review. The proposed 55 × 54 mm one-up tooling carrier has three Ø2 mm tooling holes; JLC must confirm precision routing, relief machining, and final removal of the temporary tabs. Either an accepted ≤0.80 mm cutter or an accepted drill-and-route process can form the reliefs. Do not substitute larger internal radii automatically. The order review can resolve this through controlled fabrication dimensions; an unavailable physical dry-fit is not a prerequisite. [JLCPCB routing capabilities](https://jlcpcb.com/capabilities/pcb-capabilities)

All 37 populated footprints have STEP models. J1 uses the exact configured Samtec/CADENAS model, retained unmodified with its attribution and license under `hardware/3dmodels/`; the other parts use stock package models. U1 and Y1 use package-envelope stand-ins scaled to the nominal 0.75 mm and 0.90 mm body heights. TP1–TP3 are exposed copper pads and appear as pad geometry in the STEP export. [ADI package outline](https://mds.analog.com/api/public/content/tqfn_21-0140.pdf) · [Taitien package drawing](https://www.taitien.com/wp-content/uploads/2015/12/XO-0076_TX.pdf)

The RF receiver, reference, PLL filter and their local bypass geometry are shifted inward to accommodate the notch. The power entry and host connectors remain in place. The signal bridges are rerouted to preserve short paths and orderly spacing; the RF and clock return planes are checked after refill.

The cleaned PCB passed KiCad 10 DRC with all track errors and schematic parity enabled: 0 violations, 0 unconnected items and 0 parity issues. It contains 159 ground vias. A native refill comparison found identical physical filled copper; only two redundant polygon vertices differed. All filling/capping flags are now disabled as requested, with existing mask tenting preserved. Before ordering, confirm the exact controlled-impedance stack, ENIG and notch machining in CAM. Launch response, rail ripple, PLL lock, QFN assembly yield and capture timing remain prototype measurements; layout calculations are not bench results.

## Silkscreen

The board and root schematic are titled **Ephemeris 1**, Rev 1.0. The front uses the selected unboxed Optima Bold wordmark, centered at X = 97.5 mm. Native filled outlines reinforce the fine connectors by 0.025 mm. The brand’s left ink margin and the revision’s right ink margin are both 2.695 mm, with 2026 centered below rivercraft. TP1 is marked Lock, TP2 is marked 3V, and TP3 uses a ground symbol to its left. The compact test-point cluster and nearby labels follow the user's cleanup. J3 marks +5V and ground. The host header labels I1, I0, Q1, Q0, CLK, active-low CS, SCK, SDI and VIO; a ground symbol above its even-pin column identifies pins 2–14 as ground. Pin 16 is HOST_VIO, not ground.

The back engraving combines Earth's limb, a star field, four consistently drawn GNSS spacecraft and a square tensioned mirror without a dish. Four straight ranging connections terminate at the mirror's central receiver. Each navigation spacecraft has a short, restrained local trajectory derived from a projected circular orbit centered on the illustrated Earth. The complete ellipses are not drawn; the relative orbital scale is compressed for composition, not an altitude or ephemeris prediction. Construction parameters are retained in [Ephemeris-1-orbits.json](../hardware/art/Ephemeris-1-orbits.json).

The [image source](../hardware/art/Ephemeris-1-source.png) was developed with the built-in image generation tool. Engraved edges and fine error-diffused tones were sampled at 600 dpi, then mapped into the 45 × 34 mm board with a 1 mm inset. The [print mask](../hardware/art/Ephemeris-1-print-mask.png) and [SVG](../hardware/art/Ephemeris-1-back.svg) include the actual outline and back-pad clearance windows. They are physical back views: the host header is on the left and SMA on the right. Local millimeter coordinates map to the PCB as x = 120 − x_svg and y = 61 + y_svg; apply that mirror exactly once if reimporting.

Native B.SilkS artwork is locked in the group `Ephemeris 1 — Celestial Atlas`: 16,521 filled polygon pieces, spatially tiled at 2 mm. The approved engraving and orbital trails are unchanged; the manufacturing mask follows the new notch and current back pads, removing the old SMA’s obsolete pad windows. A reconstruction check matched the previous approved print mask exactly before refitting. Zero-width outlines preserve the traced ink area. At the user’s direction, fine artwork texture is retained below the ordinary 0.15 mm stroke guideline; the smallest dots and gaps may soften or merge in printing. The graphic-stroke rule excludes only zero-outline geometry in this named art group. Mask/pad clearance and functional-silkscreen checks remain active.

## Verification

On November 18, 2026, KiCad 10.0.0 reported zero ERC violations, DRC violations, unconnected items and schematic-parity issues on the published design. The board contains 40 footprints, including three copper test pads. The assembly BOM matches all 37 fitted references and their schematic MPNs. The checked PCB SHA-256 is `f56cc12504e7f384b805874c55de2b3c9c6c592ef12839a322e504334e07e41a`.

Earlier reviews document the IC pin and register audit, placement changes, and exposed-pad stencil geometry. Their hashes identify the board at the time of each review. [IC audit](reviews/ic-audit.md) · [Layout and stencil review](reviews/cleanup-review.md)

The November 6 mask edit removed 18 perimeter openings and left the functional SMA and test-point openings in place. The board has no enclosure contact along that edge. [TI enclosure grounding example](https://www.ti.com/lit/an/snla474b/snla474b.pdf)
