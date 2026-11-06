# Rev A design and bring-up

## Circuit

- J1 is an Amphenol RF 132289 end-launch SMA. C1 blocks DC into LNA1. C2 AC-couples LNAOUT to MIXIN using the MAX2769B datasheet's direct path. Select LNA1 explicitly in firmware and leave antenna bias disabled. The first spin omits the interstage SAW filter.
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

The 0402 footprints keep the RF, reference and QFN bypass paths short. For U1, place C7/C8 at pin 4, C9 at pin 11, C10 at pin 13, C11 at pin 14, C12 at pin 19, and C13 at pin 23. Route the 3.0 V branches from the regulator output region with a nearby ground return at each pin. The 470 pF RF couplers follow ADI's typical application. Use the capacitor's RF model with the actual launch and interstage geometry before release. [ADI typical application](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf) · [Murata 470 pF model](https://ds.murata.com/simsurfing/mlcc.html?partnumbers=%5B%22GRM1555C1H471JA01%22%5D)

TI calls for at least 1 µF effective output capacitance. After the 4.7 µF part's ±10% tolerance and ±15% X7R allowance, the simple remaining budget is 3.60 µF before DC bias and aging. A loss of about 72% from that figure would reach 1 µF. Verify the effective value at 3.0 V against the chosen lot or vendor bias data if substituting. [TI TPS7A20](https://www.ti.com/lit/ds/symlink/tps7a20.pdf) · [TDK characterization](https://product.tdk.com/system/files/dam/doc/product/capacitor/ceramic/mlcc/charasheet/c2012x7r1e475k125ab.pdf)

## Footprints and hot-plate assembly

- **U1** uses ADI drawing 90-0023 Rev C for T2855+3. The 28 perimeter lands are 0.95 × 0.30 mm on 0.50 mm pitch with opposing rows 4.68 mm apart. The 3.25 × 3.25 mm ground paddle is pad 29 with 0.24 mm nominal clearance to the perimeter lands. [MAX2769B datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf) · [ADI package outline](https://mds.analog.com/api/public/content/tqfn_21-0140.pdf) · [ADI land pattern](https://mds.analog.com/api/public/content/90-0023.pdf)
- **U2–U4 and Y1** — TPS7A20 DBV pin functions are IN 1, GND 2, EN 3, NC 4 and OUT 5. The TXU0104 and TXU0304 use TI's PW TSSOP-14 pin maps, including their different channel-4 directions. The Taitien TX footprint uses 1.00 × 0.90 mm pads on 2.40 × 1.80 mm centers. Its pin 1 is lower left in the top view, with GND on 2, OUT on 3 and VDD on 4. [TI regulator](https://www.ti.com/lit/ds/symlink/tps7a20.pdf) · [TI TXU0104](https://www.ti.com/lit/ds/symlink/txu0104.pdf) · [TI TXU0304](https://www.ti.com/lit/ds/symlink/txu0304.pdf) · [Taitien drawing](https://www.taitien.com/wp-content/uploads/2015/12/XO-0076_TX.pdf)
- **Stencil** — Start with 0.10 mm laser-cut stainless and Type 3 or finer no-clean paste. Use the paste vendor's reflow profile and measure the PCB temperature at the QFN with a thermocouple. U1 has four rounded 1.31 mm square paste openings over the paddle, about 63% area coverage. Keep the paddle soldered to ground and use a short path into L2. Choose an approved filled and capped process for vias in the paste area, or place the ground vias outside the paste windows without creating a long return path. [ADI QFN assembly guidance](https://www.analog.com/en/resources/app-notes/smt-assembly-and-pcb-design-guidelines-for-maxims-standard-wirebonded-quad-flatpack.html)
- **Connectors** — Hand-solder J1, J2 and J3 after SMT reflow. J1's local footprint uses the Amphenol copper dimensions and omits paste. J2 and J3 retain the 2.54 mm pitch and use Würth's nominal 1.10 mm finished holes. Verify the fab's finished-hole tolerance. [Amphenol 132289](https://www.amphenolrf.com/en-us/part/132289/1030/) · [Würth J2](https://www.we-online.com/components/products/datasheet/61301621121.pdf) · [Würth J3](https://www.we-online.com/components/products/datasheet/61300211121.pdf)

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

1. Apply current-limited 5 V and verify the 3.0 V rail. Supply host VIO and use a common ground. Bring up with short header wiring. Check 16.368 MHz on HOST_CLK after programming and confirm PLL lock on TP1.
2. Write all ten MAX2769B registers after power-up. Use a 32-bit MSB-first transaction with CS low, sending 28 data bits followed by the four address bits. The device samples SDATA on rising SCLK and latches on rising CS. Start with a slow SCLK such as 1 MHz and hold CS high for at least a full clock period between frames.
3. Use CONF2.IQEN and enable both PGAs with CONF3[13:12] = 11. Keep the interface in parallel CMOS mode. CONF3 bit 12 is identified by ADI's clarification. Datasheet Table 9 labels it reserved.
4. Feed a known L1 test signal at a suitable receiver input level. Verify the I and Q lines toggle, then inspect clock and data timing at J2. Choose the capture edge and PIO timing from the measured data-valid window because clock and data take different translator paths.
5. Put I1, I0, Q1 and Q0 on consecutive RP2350B GPIOs in the same PIO window. Use `IN PINS, 4` with autopush after 32 bits and DMA into RAM. Start with a bounded capture and check sample alignment, spectrum and clipping. 16.368 MS/s × 4 bits = 65.472 Mb/s, or 8.184 MB/s packed. A 256 KiB buffer holds about 32 ms. Continuous delivery depends on the host's output transport and buffering.

### Register starting point

These are candidate 28-bit values derived from MAX2769B Tables 6–16. They require bench verification. Pack a transaction as `(data << 4) | address`. The sign/magnitude mapping is in Table 2.

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

The 45 × 34 mm routed outline runs from (75, 61) to (120, 95) in KiCad. The grid origin is at the upper left and the drill/place origin marker is at the lower left. The board contains the schematic footprints and ratsnest for placement and routing.

Use JLCPCB's nominal 1.20 mm, four-layer FR-4 3313 impedance stack. The stack uses 1 oz outer copper and 0.5 oz inner copper. Confirm the selected stack and finished thickness in the fabrication quote. [JLCPCB 1.2 mm JLC3313 stack table](https://cart.jlcpcb.com/client/template/placeOrder/impedance.html) · [JLCPCB calculator inputs](https://jlcpcb.com/help/article/user-guide-to-the-jlcpcb-impedance-calculator)

| Layer or material | Thickness (mm) | Layout use |
| --- | ---: | --- |
| L1 / F.Cu | 0.035 | RF, parts and local signal routing |
| 3313 prepreg | 0.0994 | L1 to L2, design Dk ≈ 4.1 |
| L2 / In1.Cu | 0.0152 | Solid GND reference |
| FR-4 core | 0.865 | L2 to L3, design Dk ≈ 4.6 |
| L3 / In2.Cu | 0.0152 | Power distribution and low-speed routes as needed |
| 3313 prepreg | 0.0994 | L3 to L4 |
| L4 / B.Cu | 0.035 | Secondary signals and ground |

KiCad models 0.01524 mm solder mask over copper on each face. The modeled stack totals about 1.195 mm.

- **RF routing.** Start with 0.20 mm trace width and a 0.20 mm gap to top ground on the `SE_50` nets. This is a grounded coplanar estimate of about 50.5 Ω with 0.0994 mm to L2, 0.035 mm copper and Dk 4.1. The estimate uses a straight uniform line in air above the board. Confirm the final mask-covered width and gap with the fab's impedance tool. Stitch the top ground to L2 around the launch and taper into the SMA land. [Grounded coplanar model](https://scikit-rf-official.readthedocs.io/en/latest/_modules/skrf/media/cpw.html) · [JLCPCB impedance calculator](https://jlcpcb.com/impedance)
- **J1 seating.** Align the dashed `User.Drawings` line on J1 with the left `Edge.Cuts` line. The top signal land is 5.20 × 1.50 mm. The four ground lands are 4.90 × 1.60 mm on ±4.25 mm centers. All lands end at the same inboard line. Their nominal front edges sit 0.40 mm (signal) and 0.70 mm (ground) from the routed edge. The 0.40 mm signal setback leaves 0.20 mm if a regular-tolerance routed edge moves 0.20 mm inward. KiCad checks 0.30 mm minimum copper-to-edge clearance with no launch exception. [Amphenol 132289 drawing](https://datasheet.lcsc.com/datasheet/pdf/aecd7a13550ce04adaae36681a1a5d95.pdf) · [JLCPCB edge tolerances](https://jlcpcb.com/capabilities/Capab)
- **J1 assembly fit.** At the fab's published ±10% thickness tolerance, the board is 1.08–1.32 mm versus the connector's 1.57 mm maximum. The drawing uses 1.57 mm PCB thickness and shows a nominal 1.73 mm gap between tabs. Dry-fit on 1.2 mm stock and confirm the center pin seats on top and both underside tabs can make solid solder joints before committing to this connector on the finished PCB. [Amphenol product specifications](https://www.amphenolrf.com/en-us/part/132289/1030/) · [JLCPCB thickness tolerance](https://jlcpcb.com/resources/pcb-thickness)

Before fabrication, verify the launch and capacitor impedance at GPS L1, finish the routing and planes, and rerun DRC and schematic parity on the completed board.

## References

- [Analog Devices MAX2769B datasheet, Rev. 2](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf) — typical application, pin descriptions, registers and layout
- [Analog Devices MAX2769BEVKIT, Rev. 0](https://www.analog.com/media/en/technical-documentation/data-sheets/max2769bevkit.pdf) — schematic on p. 5, PCB artwork on pp. 6–9
- [ADI Q PGA clarification](https://ez.analog.com/rf/a/documents/c/max2769beti-v-_t1-faq/DO25034/max2769b-enabling-i-and-q-outputs-using-configuration-register-3) and [power-up register initialization](https://ez.analog.com/rf/a/documents/do25035/max2769b-power-up-register-initialization)
- [ADI package drawing 21-0140](https://www.analog.com/media/en/package-pcb-resources/package/pkg_pdf/tqfn/21-0140.pdf)
- [TI TPS7A20](https://www.ti.com/lit/ds/symlink/tps7a20.pdf), [TXU0104](https://www.ti.com/lit/ds/symlink/txu0104.pdf) and [TXU0304](https://www.ti.com/lit/ds/symlink/txu0304.pdf)
- [Taitien TX oscillator drawing](https://www.taitien.com/wp-content/uploads/2015/12/XO-0076_TX.pdf)
- [Amphenol RF 132289](https://www.amphenolrf.com/en-us/part/132289/1030/)
- [Raspberry Pi RP2350 datasheet](https://pip-assets.raspberrypi.com/categories/1214-rp2350/documents/RP-008373-DS-2-rp2350-datasheet.pdf?disposition=inline)

The local connector and QFN footprints are adapted from the [KiCad footprint libraries](https://www.kicad.org/libraries/license/).
