# GNSS I/Q Breakout

Rev A is a GPS L1 bench front end for the MAX2769BETI/V+T. It accepts a passive antenna or an externally biased RF feed through a DC-blocked 50 Ω SMA input and exposes parallel I/Q and configuration on a ground-interleaved host header. The first capture target is an external RP2350B using PIO and DMA.

Open [the KiCad 10 project](hardware/GNSS-IQ-Breakout.kicad_pro). The root sheet connects **RF and ADC**, **Host Interface**, and **Power Entry**. The `{IQ}` and `{CFG}` bus aliases break out on the child sheets. The board file contains an unrouted four-layer placement draft.

- [Design sources, header and bring-up](docs/design.md)
- [Project drawing and verification conventions](AGENTS.md)
