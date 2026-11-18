# Ephemeris 1 — user cleanup review

Reviewed 2026-11-06 against the previous checked design. The cleanup preserves the circuit and improves several local routes. The drawing and layout preferences observed here are now recorded in `AGENTS.md`. This report records the cleanup before the later mask-only perimeter-border removal; its source hash and export checks are historical. The design record identifies the published board and its checks. [Verification](../design.md#verification)

## Schematic changes

The PLL filter is drawn more compactly: C5/C6 moved upward, C6 moved inward, and both ground returns shortened. Capacitor bodies and ground symbols retain common baselines. The branch spacing decreased from 20.32 to 16.51 mm; the top-junction-to-ground span decreased from 16.51 to 11.43 mm. Ground-value labels sit closer to their symbols, while Y1 gained a short visible ground lead to separate its ground symbol from the component pin.

The fresh exported netlist matches the previous review exactly: **40 components and 45 nets**, with unchanged values, MPNs, footprints, datasheets, net memberships and pin functions. The root, host-interface and power-entry sheets are unchanged. The rendered RF sheet has no new label collision or ambiguous connection.

The durable preference is compact, visibly connected circuitry with aligned repeated elements and minimal label padding. Future edits should preserve this spacing rather than expanding small filters or restoring long ground tails.

## PCB changes

- R8–R10 moved into their actual control-signal routes. Routed centerline totals fell from 34.97 to 13.44 mm for CS_N, 20.17 to 14.84 mm for SCLK, and 21.45 to 11.93 mm for SDATA. Both SCLK layer transitions were eliminated.
- Nearby capacitors and filter parts form local aligned pairs and columns. R1 turned vertically to make the PLL group narrower. That group trades slightly more branched PLL_CP trace length for a smaller overall arrangement; its topology and via-free control-node routing are preserved.
- C7 moved closer to VCC_RF. The receiver bypass ground returns remain short, with ground-pad-center-to-nearest-via distances of approximately 0.52–0.70 mm. Some other local path lengths increased slightly, so compact appearance should still be weighed against the specific electrical loop.
- TP2/TP3 and their labels form a tighter cluster. The ground symbol sits to the left of TP3 and the rail label to the right of TP2. The slogan moved right/up; the lock label became heavier, and `3V0` became `3V`.
- The minimum via hole was raised from 0.25 to 0.30 mm in both project settings and actual geometry. The cleaned board has 35 vias at 0.50/0.30 mm and 151 at 0.60/0.30 mm, preserving a 0.10 mm minimum annular ring.

The cleaned board contains **186 vias, including 159 ground vias**, versus 199/169 previously. Nine signal-layer transitions remain. Native filled-copper checks show all ground via centers connected to ground copper on all four layers. The major ICs, oscillator, connectors, outline, stack, SMA notch and zone boundaries are unchanged. RF traces remain 0.15 mm wide on the front layer. All 37 populated component model references resolve.

The layout preference is to retain major-device symmetry while aligning smaller parts locally and keeping them on their actual signal routes. Do not add routing detours merely to maintain a decorative row. Keep test-point labels and ground symbols close to the functions they identify.

## Review correction

The saved file preserved the artwork geometry but lost part of the back-art and title group memberships. The missing back-art membership produced 199 DRC messages because those polygons no longer matched the existing narrow artwork-stroke exception. Restored only the membership metadata: 2,494 back-art and 317 title members. All artwork polygons, copper and drawing rules were preserved. This is separate from the user's electrical/layout cleanup.

## Ordinary vias and stencil assembly

The user requested **no filling or copper capping**. All six remaining fill/cap flags were disabled. The final board has **zero filled/capped vias**, and the fabrication package contains no fill/cap process maps. Earlier six- and nine-via processing maps and ZIPs are superseded.

U1 keeps its five 0.60/0.30 mm plated exposed-pad ground vias. The existing bottom solder mask tents these holes; the front is open through the exposed-pad solder window. Mask tenting is not a guaranteed seal or a flat copper cap. ADI describes tenting as a thermal-via solder-control option, so the earlier blanket fill/cap requirement was too strong. [ADI assembly guidance](https://www.analog.com/en/resources/app-notes/an-772.html)

The former paste apertures missed the four off-center holes by only 0.005 mm. To give stencil alignment useful margin, the four **paste-only** rounded squares now measure 1.25 × 1.25 mm at local centers (±0.925, ±0.925) mm, with 0.25 mm corner radius. This gives **0.150 mm nominal clearance to the four off-center hole edges**, 0.378 mm to the center hole and **57.14% paddle coverage**. Copper, solder-mask openings, vias, routing and placement are unchanged. The local library and placed U1 footprint carry the same stencil refinement. Keep the 0.10 mm stencil and controlled paste deposit; adding excess paste beneath U1 can cause lifting or bridging. Small-hole geometry and separated paste reduce direct paste loss, but the first hotplate assembly still needs inspection for solder loss/voiding. [Maxim QFN stencil guidance](https://www.analog.com/en/resources/app-notes/smt-assembly-and-pcb-design-guidelines-for-maxims-standard-wirebonded-quad-flatpack.html)

The cleanup keeps other SMT solder openings clear of drilled holes. The eight front-exposed SMA ground-land vias remain ordinary plated holes for hand soldering after SMT reflow; J1 has no stencil apertures. The board's ordinary mask tenting is retained elsewhere.

The Samtec precision-notch geometry and controlled-impedance stack remain unchanged. The final fabrication notes must retain their machining/stack requirements and use newly generated data from the final checked board.

## Cleanup verification before border removal

- Cleanup PCB SHA256: `a6a36cd9d58d03cbeb125550e48dc9d8445905859a06289439721e8758c79070`.
- Fresh KiCad 10 checks: **0 ERC violations, 0 DRC violations, 0 unconnected items, 0 schematic-parity issues**, under the existing project rules. The narrowly scoped fine-art stroke exception is unchanged.
- All 40 components and 45 nets match the previous circuit. All 37 populated parts have resolving 3D references; the three test pads are native copper geometry.
- Minimum trace: **0.15 mm**. Minimum copper clearance: **0.20 mm**. Minimum via: **0.50 mm land / 0.30 mm hole**, **0.10 mm annulus**. The unused saved 0.45/0.25 mm preset remains below the new drill minimum and must not be used.
- **230 straight tracks, 31 arcs, 186 vias, 159 GND vias, 8 zones**. Nine signal transitions remain; the maximum distance to a nearby ground via is 1.693 mm.
- Native refill comparison: no physical copper difference on any of seven filled zone/layer sets. Two redundant front-ground polygon vertices disappear on refill, with an empty geometric XOR.
- Final Gerbers and stencil are regenerated; drill data contains **204 plated holes** (186 vias and 18 header holes), zero NPTH. No fill/cap processing files remain in the current fabrication ZIP.
- Front/back renders and RF schematic inspected. Your placement, routes, copper pads, outline, stack, mask and artwork geometry are preserved; the review changes are group membership, via process flags and U1 paste-only geometry.

RF response, rail noise, CLKOUT voltage margin and capture timing remain first-prototype measurements. The machining tolerance for the Samtec notch and the specified controlled-impedance stack are fabrication requirements. [Design and bring-up](../design.md)
