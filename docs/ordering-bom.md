# Ephemeris 1 parts and assembly

One board takes **37 parts across 18 order lines**, all placed on the front. TP1–TP3 are bare copper. Use the interactive BOM to find pin 1, highlight pads and check off placements. It shows schematic MPNs. The table below includes the purchasing substitutions marked †. [Interactive assembly BOM](assembly/ibom.html) · [Reflow and connector assembly](design.md#footprints-and-hot-plate-assembly)

## Cost per board

The listed parts total **$32.96 per board** using the installed quantities and DigiKey US quantity-one prices checked November 18, 2026. The ICs and oscillator account for $19.63, the connectors $9.25, the capacitors $3.08 and the resistors $1.00. This estimate excludes the bare PCB, stencil, spares, tax, shipping, labor, antenna and capture host.

For context, Pumpkin lists its flight-qualified GPSRM 1 receiver kit from $11,485. That kit computes position, velocity and time. Ephemeris 1 is at the raw-sample stage of an in-house receiver. [Pumpkin GPSRM 1](https://www.pumpkinspace.com/store/p58/GNSS_Receiver_Module_%28GPSRM_1%29_Kit.html)

## Order lines

Prices are USD per part. The two-board buy quantities allow for hand-assembly spares. Use cut tape for SMT parts and single pieces for connectors.

| References | Part | Purchase MPN · DigiKey | DigiKey SKU | Qty / board | Buy for 2 with spares | Unit price |
| --- | --- | --- | --- | ---: | ---: | ---: |
| U1 | GNSS RF receiver, TQFN-28 | [MAX2769BETI/V+T](https://www.digikey.com/en/products/detail/analog-devices-inc-maxim-integrated/MAX2769BETI-V-T/2793585) | 175-MAX2769BETI/V+TCT-ND | 1 | 2 | $14.28 |
| U2 | 3.0 V LDO, SOT-23-5 | [TPS7A2030PDBVR](https://www.digikey.com/en/products/detail/texas-instruments/TPS7A2030PDBVR/13566938) | 296-TPS7A2030PDBVRCT-ND | 1 | 2 | $0.35 |
| U3 | 4-channel I/Q translator, TSSOP-14 | [TXU0104QPWRQ1](https://www.digikey.com/en/products/detail/texas-instruments/TXU0104QPWRQ1/14641448) † | 296-TXU0104QPWRQ1CT-ND | 1 | 2 | $1.30 |
| U4 | Control/clock translator, TSSOP-14 | [TXU0304PWR](https://www.digikey.com/en/products/detail/texas-instruments/TXU0304PWR/14641423) | 296-TXU0304PWRCT-ND | 1 | 2 | $1.05 |
| Y1 | 16.368 MHz TCXO, 3.2 × 2.5 mm | [TXETCDSANF-16.368000](https://www.digikey.com/en/products/detail/taitien/TXETCDSANF-16-368000/6127629) | 1664-TXETCDSANF-16.368000CT-ND | 1 | 2 | $2.65 |
| J1 | Samtec SMA jack, EM3 notch mount | [SMA-J-P-H-ST-EM3](https://www.digikey.com/en/products/detail/samtec-inc/SMA-J-P-H-ST-EM3/6678687) | SAM9950-ND | 1 | 2 | $8.12 |
| J2 | 2×8 male header, 2.54 mm | [61301621121](https://www.digikey.com/en/products/detail/w%C3%BCrth-elektronik/61301621121/4846856) | 732-5299-ND | 1 | 2 | $1.00 |
| J3 | 1×2 male header, 2.54 mm | [61300211121](https://www.digikey.com/en/products/detail/w%C3%BCrth-elektronik/61300211121/4846823) | 732-5315-ND | 1 | 2 | $0.13 |
| C1, C2, C6 | 470 pF, C0G, 50 V, 5%, 0402 | [GRM1555C1H471JA01D](https://www.digikey.com/en/products/detail/murata-electronics/GRM1555C1H471JA01D/587921) | 490-1297-1-ND | 3 | 10 | $0.10 |
| C3 | 10 nF, X7R, 50 V, 10%, 0402 | [GRM155R71H103KA88D](https://www.digikey.com/en/products/detail/murata-electronics/GRM155R71H103KA88D/1033252) | 490-4516-1-ND | 1 | 10 | $0.10 |
| C5 | 27 pF, C0G, 50 V, 5%, 0402 | [GRM1555C1H270JA01D](https://www.digikey.com/en/products/detail/murata-electronics/GRM1555C1H270JA01D/3175204) | 490-5869-1-ND | 1 | 10 | $0.10 |
| C8 | 100 pF, C0G, 50 V, 5%, 0402 | [GRM1555C1H101JA01J](https://www.digikey.com/en/products/detail/murata-electronics/GRM1555C1H101JA01J/4358006) † | 490-7754-1-ND | 1 | 10 | $0.10 |
| C7, C9, C10, C11, C12, C13 | 100 nF, X7R, 16 V, 10%, 0402 | [GRM155R71C104KA88J](https://www.digikey.com/en/products/detail/murata-electronics/GRM155R71C104KA88J/3845525) † | 490-6328-1-ND | 6 | 20 | $0.10 |
| C4, C14, C15, C16, C17 | 100 nF, X7R, 16 V, 10%, 0603 | [CC0603KRX7R7BB104](https://www.digikey.com/en/products/detail/yageo/CC0603KRX7R7BB104/302998) † | 311-1088-1-ND | 5 | 20 | $0.14 |
| C18, C19 | 4.7 µF, X7R, 35 V, 10%, 0805 | [C2012X7R1V475K125AC](https://www.digikey.com/en/products/detail/tdk-corporation/C2012X7R1V475K125AC/2732990) † | 445-7555-1-ND | 2 | 10 | $0.59 |
| R1 | 20 kΩ, 1%, 0.1 W, 0603 | [RC0603FR-0720KL](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0720KL/729987) | 311-20.0KHRCT-ND | 1 | 10 | $0.10 |
| R2, R3, R4, R5, R6 | 22 Ω, 1%, 0.1 W, 0603 | [AC0603FR-0722RL](https://www.digikey.com/en/products/detail/yageo/AC0603FR-0722RL/5895927) † | YAG3581CT-ND | 5 | 20 | $0.10 |
| R7, R8, R9, R10 | 47 kΩ, 1%, 0.1 W, 0603 | [RC0603FR-0747KL](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0747KL/730200) | 311-47.0KHRCT-ND | 4 | 10 | $0.10 |

## Purchasing substitutions

The original ordering review on November 6, 2026 recorded the following substitutions. See the linked listings for current availability.

- U3: TXU0104PWR was backordered at review; TXU0104QPWRQ1 uses the same PW TSSOP-14 footprint/pin map, four A→B channels, active-high OE and suitable rail range. Automotive qualification is added. [Original TI datasheet](https://www.ti.com/lit/ds/symlink/txu0104.pdf) · [Q1 datasheet](https://www.ti.com/lit/ds/symlink/txu0104-q1.pdf)
- C8 and C7/C9–C13: the Murata D→J suffix changes the reel packaging; electrical values and bodies are unchanged. The listed DigiKey cut-tape SKUs avoid buying reels.
- C4/C14–C17: obsolete Murata GRM188R71C104KA01D is replaced by Yageo CC0603KRX7R7BB104, matching 100 nF, ±10%, X7R, 16 V and 0603 body.
- R2–R6: backordered RC0603FR-0722RL is replaced by automotive AC0603FR-0722RL, matching 22 Ω, ±1%, 0.1 W, 0603 and ±100 ppm/°C.
- C18/C19: backordered TDK C2012X7R1E475K125AB is replaced by C2012X7R1V475K125AC, 4.7 µF ±10%, X7R, **35 V** rather than 25 V, with the same nominal 2.0 × 1.25 × 1.25 mm body. Reading the exact TDK characterization curve supports conservative 20% loss at 5.25 V and 10% at 3.045 V; after −10% tolerance and −15% temperature, estimates are **2.88 µF input / 3.24 µF output before aging**. These typical-curve estimates preserve the previous capacitor margin, not a guaranteed worst-case capacitance. [TDK characterization](https://docs.rs-online.com/1d3c/A700000006893460.pdf)
