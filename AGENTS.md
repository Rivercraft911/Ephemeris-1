# GNSS I/Q Breakout working instructions

Adapted from the IREC working instructions that cover Pigeon Vision. Use the PigeonCarrier hierarchy and Yapogee S3 RF and power sheets as drawing references.

## Engineering

- Verify the exact MPN, package variant, pad numbering, voltage limits and register fields against the manufacturer sources in [the design notes](docs/design.md). Trace the actual netlist across the schematic hierarchy before changing RF or host connections.
- Separate datasheet limits, calculated values and measured results. Record the inputs and units next to simple calculations.
- Keep the SMA launch, RF coupling, crystal reference, PLL filter and power bypass loops short. Check the selected PCB stackup, connector thickness and assembly process before routing or releasing fabrication files.

## KiCad

- Make the root sheet a readable block diagram. Join related signals with named KiCad bus aliases, matching hierarchical labels and sheet pins, and explicit bus entries on the child sheets.
- Organize child sheets by circuit function. Keep wires short and draw bypass caps, pulls and filters into their circuits. Use short labels where direct wiring would obscure the sheet. Keep notes sparse and put sizing, sourcing and assembly detail in the design notes.
- Use uppercase underscore-delimited nets and the `_N` suffix for active-low signals. Use `SE_50` for the single-ended RF path and record the actual trace geometry once the stackup is selected.
- Preserve stable references and project-relative library paths. Record MPNs, footprints and relevant datasheets in the schematic or design notes.

## Files and checks

- Keep editable KiCad sources under `hardware/` and bench notes under `docs/`. Put local plots, checks and manufacturing exports under `build/` unless the user requests a release package.
- Run schematic ERC and check the exported netlist after electrical edits. Check board-to-schematic parity, clearance and connectivity after layout changes. Inspect rendered sheets for label collisions and wire crossings.
- Record remaining design and bench checks before advancing the revision. Preserve unrelated work and user edits.
