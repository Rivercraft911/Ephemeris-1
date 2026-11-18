# Ephemeris 1

Tired of $10,000 spacecraft GNSS quotes? About $33 in parts buys the front end of our own GPS L1 receiver. Some soldering required.

![Angled views of Ephemeris 1 in black with white silkscreen. The front has the SMA antenna input and I/Q header. The back shows four navigation satellites above Earth.](https://github.com/Rivercraft911/Ephemeris-1/releases/download/first-boards/ephemeris-1.png)

The MAX2769B takes GPS L1 in through the SMA and sends raw I/Q samples to the host header. The first boards are here. Next up is capture on an RP2350B, then hardware-in-the-loop testing with a Safran GNSS simulator.

[Interactive assembly BOM](https://rivercraft911.github.io/Ephemeris-1/assembly/ibom.html) · [Parts and cost](docs/ordering-bom.md) · [KiCad](hardware/GNSS-IQ-Breakout.kicad_pro) · [Bring-up](docs/design.md#first-capture)
