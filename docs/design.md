# Rev A design and bring-up

## Circuit

- J1 is an Amphenol RF 132289 end-launch SMA. C1 blocks DC into LNA1. C2 AC-couples LNAOUT to MIXIN using the MAX2769B datasheet's direct path. Select LNA1 explicitly in firmware and leave antenna bias disabled. The first spin omits the interstage SAW filter.
- Y1 is a 16.368 MHz clipped-sine TCXO on the receiver supply, AC-coupled to XTAL. The PLL filter uses 27 pF in parallel with a series 20 kΩ / 470 pF branch. The values follow the datasheet starting point for a 1.023 MHz comparison frequency and 0.5 mA charge pump.
- J3 takes regulated 5 V on pin 1 and ground on pin 2. U2 produces 3.0 V, with a static tolerance range of 2.955–3.045 V at ±1.5%. The MAX2769B operating range is 2.7–3.3 V. TP2 and TP3 are the receiver rail and ground. TP1 exposes the PLL lock indication.
- The MAX2769B is the T2855+3 package, 5 × 5 mm, 0.5 mm pitch and nominal 3.25 mm exposed pad. U1 pad 29 maps to the exposed ground paddle. The footprint uses the KiCad T2855-3 land pattern. Add ground vias with the assembler's approved stencil and via process.
- U3 translates the four CMOS sample bits to host VIO. U4 translates the three configuration lines toward the receiver and the reference clock toward the host. Use 3.3 V host VIO for initial bring-up. Verify a different host's I/O voltage, thresholds and timing before connecting it. R2–R6 are initial source damping values.

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

## Before fabrication

The PCB has all schematic footprints and nets placed on a working outline. Select a four-layer stackup with a continuous L2 ground plane. Confirm finished thickness and tolerances for the 132289, which Amphenol specifies for up to 1.57 mm. Set the 50 Ω geometry from the fab stackup and review the launch with the connector drawing. Complete the RF routing, stitching, decoupling returns, remaining routing and planes. Run DRC and schematic parity on the final board. Confirm the RF capacitor impedance at L1, TCXO land pattern, effective LDO output capacitance and complete orderable BOM before release.

## References

- [Analog Devices MAX2769B datasheet, Rev. 2](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf) — typical application, pin descriptions, registers and layout
- [Analog Devices MAX2769BEVKIT, Rev. 0](https://www.analog.com/media/en/technical-documentation/data-sheets/max2769bevkit.pdf) — schematic on p. 5, PCB artwork on pp. 6–9
- [ADI Q PGA clarification](https://ez.analog.com/rf/a/documents/c/max2769beti-v-_t1-faq/DO25034/max2769b-enabling-i-and-q-outputs-using-configuration-register-3) and [power-up register initialization](https://ez.analog.com/rf/a/documents/do25035/max2769b-power-up-register-initialization)
- [ADI package drawing 21-0140](https://www.analog.com/media/en/package-pcb-resources/package/pkg_pdf/tqfn/21-0140.pdf)
- [TI TPS7A20](https://www.ti.com/lit/ds/symlink/tps7a20.pdf), [TXU0104](https://www.ti.com/lit/ds/symlink/txu0104.pdf) and [TXU0304](https://www.ti.com/lit/ds/symlink/txu0304.pdf)
- [Taitien TX oscillator drawing](https://www.taitien.com/wp-content/uploads/2015/12/XO-0076_TX.pdf)
- [Amphenol RF 132289](https://www.amphenolrf.com/en-us/part/132289/1030/)
- [Raspberry Pi RP2350 datasheet](https://pip-assets.raspberrypi.com/categories/1214-rp2350/documents/RP-008373-DS-2-rp2350-datasheet.pdf?disposition=inline)
