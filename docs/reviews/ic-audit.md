# Ephemeris 1 Rev 1.0 — IC implementation review

> Historical electrical audit. The later [user cleanup review](cleanup-review.md) preserves this circuit/netlist but supersedes this report's layout counts, local distances, source hash and fabrication package. The current board uses 0.30 mm minimum via holes and **no filled/capped vias**; follow [current assembly notes](../design.md#footprints-and-hot-plate-assembly).


Reviewed 2026-11-06. **No required schematic, component-selection, or PCB correction was found. The design is suitable for a first prototype run, subject to the existing fabrication requirements below.** Clock voltage margin and end-to-end capture timing remain explicit qualification items; this review is not a full-temperature performance qualification.

The review used the current schematic's exported XML netlist, native PCB pad nets and geometry, exact ordering codes, and manufacturer datasheets. Separate reviews covered the receiver, regulator, and translators/reference. The source board was left unchanged, including the eight additional ground vias in the latest user save.

PCB SHA256: `e4fd47b5f71518f932b582ceadfd0fdfe1c20f51ed7ff01791b25b9396212630`.

## Device results

| Device | Implementation checked | Result |
| --- | --- | --- |
| U1 MAX2769BETI/V+T | Every pin, six supplies, seven bypasses and individual ground returns, operating straps, NC/EP, RF couplers, reference and PLL filter | Pin mapping and topology pass. Embedded T2855+3 / 90-0023 lands match the manufacturer drawing. |
| U2 TPS7A2030PDBVR | DBV pin map, EN tied to input, NC, 5 V input, 3 V output, capacitor selection and local loops | Pass. 2.955–3.045 V static rail interval; 50 mA allowance gives about 115 mW regulator dissipation. |
| U3 TXU0104PWR | Four I/Q channels, RX/host supplies, OE, NCs, bypasses, source resistors and PW footprint | Pass. I1/I0/Q1/Q0 retain their intended order. |
| U4 TXU0304PWR | Three configuration channels toward U1, reverse clock channel toward host, swapped supply roles, OE and defaults | Pass. OE can reference either supply. Clock qualification is discussed below. |
| Y1 TXETCDSANF-16.368000 | Exact variant, package numbering, supply, NC pin, clipped-sine output and AC coupling | Pass. Selected variant is rated −30 to +85 °C. Allow oscillator startup before configuration. |

Manufacturer references: [MAX2769B](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf), [ADI land pattern](https://mds.analog.com/api/public/content/90-0023.pdf), [TPS7A20](https://www.ti.com/lit/ds/symlink/tps7a20.pdf), [TXU0104](https://www.ti.com/lit/ds/symlink/txu0104.pdf), [TXU0304](https://www.ti.com/lit/ds/symlink/txu0304.pdf), [Taitien TX](https://www.taitien.com/wp-content/uploads/2015/12/XO-0076_TX.pdf), [Taitien ordering guide](https://www.taitien.com/wp-content/uploads/2015/12/Model-Numbering-Guide_TCXO-Rev307-2015.pdf).

## Calculations and physical implementation

C18/C19 are the specified TDK 4.7 µF, 25 V X7R parts. Bias, tolerance and temperature estimates give approximately 2.88/3.24 µF before aging, comfortably above the regulator's effective output-capacitance floor. At the 50 mA budget, the estimated regulator rise is 21 °C using TI's reference-board thermal resistance; actual temperature and rail noise must be measured. [TDK exact-part characterization](https://product.tdk.com/system/files/dam/doc/product/capacitor/ceramic/mlcc/charasheet/c2012x7r1e475k125ab.pdf)

Each U1 bypass has its own directly routed ground via, with pad-center-to-via distances of 0.65–0.75 mm. The 100 pF RF bypass is the nearest capacitor to VCC_RF. C7's 100 nF branch is less compact, approximately 3.92 mm geometric distance from the supply pad through the shared power pour. This is a possible future placement refinement, not a detected connectivity defect. C19 connects directly to the LDO output before the power-distribution via.

The actual 27 pF / 20 kΩ / 470 pF PLL network gives a nominal 16.93 kHz zero and 311.66 kHz pole. With 0.5 mA charge-pump current, 57 MHz/V tuning gain and N=1536, the ideal linear loop has approximately 57.3 kHz crossover and 63° phase margin. These are calculated nominal values, excluding tolerances and unmodeled IC dynamics.

## Startup registers

All ten existing 28-bit values were decoded and checked against the intended configuration. No register value changed. The configuration forces LNA1, disables antenna bias, selects both I and Q in two-bit sign/magnitude CMOS mode, and uses a 16.368 MHz reference with R=16 and N=1536. Calculated LO is 1571.328 MHz and GPS L1 IF is 4.092 MHz.

Write every register after every power-up, including TEST1/TEST2. The existing CONF3 value correctly sets bits 13 and 12, enabling both PGAs; the Q bit is mislabeled reserved in the main table and clarified by ADI. [Power-up initialization](https://ez.analog.com/rf/a/documents/do25035/max2769b-power-up-register-initialization) · [Q-channel clarification](https://ez.analog.com/rf/a/documents/c/max2769beti-v-_t1-faq/DO25034/max2769b-enabling-i-and-q-outputs-using-configuration-register-3)

Use 32-bit MSB-first frames, `(data << 4) | address`. The checked wire words for addresses 0–9 are:

```text
A2951630 855028C1 EAFF1DC2 9EC00083 0C000804
80000705 80000006 10061B27 1E0F4018 28C04029
```

## Clock margin and first-board measurements

The material limitation is the receiver-to-translator clock interface. MAX2769B specifies CLKOUT high ≥2.0 V and low ≤0.8 V at its stated load. TXU0304's threshold limits at exactly 3.0 V give only **80 mV high and 90 mV low static margin**. TI does not explicitly guarantee interpolation across the regulator's complete 2.955–3.045 V interval. Thus the available tables do not close a full-corner voltage-margin proof. The short, lightly loaded PCB connection is expected to work; that is an engineering expectation, not a measured result. [ADI clock limits](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX2769B.pdf#page=4) · [TI threshold limits](https://www.ti.com/lit/ds/symlink/txu0304.pdf#page=7)

For initial bring-up:

1. Use regulated 5 V at J3, host-supplied 3.3 V at J2 pin 16, and a common ground. TP2 is for measurement; do not use it to back-power the regulator.
2. Confirm the 3 V rail, then allow at least 2 ms for TCXO startup; a 5 ms firmware wait provides practical margin. Program all ten registers and check PLL lock at TP1.
3. Measure the receiver clock at U4 pin 10, then the translated clock and all four sample bits at J2. Check levels and edges. Clock and data pass through separate translators, so select the capture edge from the measured valid-data window.
4. Validate bounded I/Q captures and receiver response with a known L1 signal, then check continuous capture, supply ripple and temperature. Retain the first-spin limitation of no interstage SAW filter when testing blockers.

## Verification of the audited board (historical)

At this audit, KiCad 10 checks on the exact board above reported **0 DRC violations, 0 unconnected items, 0 schematic-parity issues and 0 ERC violations** under the project settings. Native inspection confirms 40 footprints, 206 straight tracks, 45 arcs, 199 vias including 169 ground vias, and 8 zones. All populated components have resolving 3D model references. The smallest trace remains 0.15 mm; smallest via is 0.45 mm copper / 0.25 mm hole.

The fabrication exports associated with this audit are superseded. Their requirement for nine filled/copper-capped vias was removed by the later cleanup; the current board uses ordinary plated vias with no filling or capping. The JLC04161H-3313 controlled-impedance stack and Samtec notch at ±0.10 mm with its specified relief geometry still require CAM confirmation. The current board checks and assembly notes are in the design record. [Verification](../design.md#verification) · [Assembly](../design.md#footprints-and-hot-plate-assembly)
