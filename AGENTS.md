# GNSS I/Q Breakout working instructions

Adapted from the IREC working instructions that cover Pigeon Vision. Use the PigeonCarrier hierarchy and Yapogee S3 RF and power sheets as drawing references.

## Engineering

- Verify the exact MPN, package variant, pad numbering, voltage limits and register fields against the manufacturer sources in [the design notes](docs/design.md). Trace the actual netlist across the schematic hierarchy before changing RF or host connections.
- Separate datasheet limits, calculated values and measured results. Record the inputs and units next to simple calculations.
- Keep the SMA launch, RF coupling, crystal reference, PLL filter and power bypass loops short. Check the selected PCB stackup, connector thickness and assembly process before routing or releasing fabrication files.
- Use ordinary plated through vias for this revision; do not add filled/capped via processing. Keep drill holes outside soldered SMT lands while preserving short return paths. Treat intentionally designed QFN exposed-pad and hand-soldered SMA ground vias as specific exceptions, with an explicit solder-mask and paste strategy.

## KiCad

- Make the root sheet a readable block diagram. Join related signals with named KiCad bus aliases, matching hierarchical labels and sheet pins, and explicit bus entries on the child sheets.
- Organize child sheets by circuit function. Keep wires short and draw bypass caps, pulls and filters into their circuits. Use short labels where direct wiring would obscure the sheet. Keep notes sparse and put sizing, sourcing and assembly detail in the design notes.
- Use uppercase underscore-delimited nets and the `_N` suffix for active-low signals. Use `SE_50` for the single-ended RF path and record the actual trace geometry once the stackup is selected.
- Preserve stable references and project-relative library paths. Record MPNs, footprints and relevant datasheets in the schematic or design notes.
- Follow the user’s cleanup style: keep filter branches compact, align repeated capacitor and ground baselines, and keep GND labels close to their symbols. Add a short visible ground lead where direct attachment crowds a component; avoid restoring excessive wire tails or label padding.
- Preserve the main device symmetry, then place small parts in their actual signal paths. Prefer local pairs and columns over decorative rows that require routing detours. Keep ground-via connections tight and group test points with nearby power labels or ground symbols.

## Files and checks

- Keep editable KiCad sources under `hardware/` and bench notes under `docs/`. Put current local checks, BOM and manufacturing exports in `build/current/` unless the user requests a release package.
- Keep compact review evidence in `build/review-evidence/`; regenerate exports in place instead of accumulating dated copies. Do not retain board screenshots or renders in the repository.
- Preserve editable artwork, libraries, model licenses and manufacturer source evidence; these are design assets, not disposable previews.
- Run schematic ERC and check the exported netlist after electrical edits. Check board-to-schematic parity, clearance and connectivity after layout changes. Inspect rendered sheets for label collisions and wire crossings.
- Record remaining design and bench checks before advancing the revision. Preserve unrelated work and user edits.
