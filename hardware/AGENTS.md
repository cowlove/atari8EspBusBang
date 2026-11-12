# Hardware: agent workflow notes

How the KiCad work in this directory gets done headless (from a shell, no
KiCad GUI). The repo-root AGENTS.md covers firmware; this file covers boards.

## Tools (on miner6)

| Tool | Where | Notes |
|---|---|---|
| KiCad 9 | PPA install: `kicad-cli` in `/usr/bin` | not the snap (no Python, missing ICU) |
| pcbnew Python | `/usr/bin/python3` only | a venv or other Python won't find `pcbnew` |
| Symbol library | `/usr/share/kicad/symbols` | the generators' `<symbols dir>` argument |
| Freerouting 2.5 | `~/opt/freerouting/freerouting.jar` | needs Java 25+ (class file 69) |
| Java 25 | `~/opt/jre25/bin/java` | portable Temurin JRE |

`autoroute.sh` honours `FREEROUTING_JAR`, `JAVA`, `ROUTE_TIMEOUT` (s),
`OPTIMIZE=true` and `SKIP_NETS`.

## Boards and who generates what

| Directory | Schematic | Board |
|---|---|---|
| `espBusBang/` | by hand (the original) | by hand |
| `eci2Pbi/` | by hand; the ECI ↔ PBI mapping reference | by hand |
| `espBusBangSidecar/` (DevKitC carrier) | `tools/make_sidecar_schematic.py` from espBusBang | not laid out yet |
| `sidecar_pcb_xl/` (bare modules, PBI) | `tools/make_sidecar_xl_schematic.py` from espBusBangSidecar + its netlist | placed by hand/script, `autoroute.sh` |
| `sidecar_pcb_xe/` (bare modules, cart + ECI) | `tools/make_sidecar_xe_schematic.py` from the XL one | `tools/make_sidecar_xe_pcb.py` from the XL board, `autoroute.sh` |

The chain is espBusBang → espBusBangSidecar → sidecar_pcb_xl → sidecar_pcb_xe.
A change to a generated schematic goes into its generator, then flows down:
regenerate every schematic below it, in order. Each README has its
generator's command line.

- **Generated schematics are never hand-edited.** After changing a generator,
  regenerate and check: `kicad-cli sch erc`, `kicad-cli sch export netlist`,
  and compare the netlist with the previous one (the XE generator's job is to
  change only J1: diff every non-J1 pin's net).
- **A refactor must reproduce the committed file byte for byte** (`cmp`). The
  generators use deterministic UUID5s, so reruns only differ where the
  circuit does.
- **Never change a generator's UUID namespace string** (e.g.
  `atari8EspBusBang/sidecar_smd/` in make_sidecar_xl_schematic.py, kept
  across the rename): the board's footprints link to schematic symbols by
  UUID, and new UUIDs orphan every footprint.
- **Check the link after any schematic or board change:**
  `kicad-cli pcb drc --schematic-parity <board>` must report 0 parity issues.

## Editing boards

- Prefer pcbnew Python (`LoadBoard`, `FindFootprintByReference`,
  `SetPosition`, `SetOrientationDegrees`, `Flip`, `SaveBoard`) over text edits.
  `tools/pcb_place.py` does text-level moves for when pcbnew isn't available
  (footprint children are stored relative to the footprint, but pad and text
  angles are absolute, so a rotation has to touch every child).
- pcbnew Python gotchas:
  - Remove items with `board.Delete(item)`; `board.Remove()` dies on
    `thisown`.
  - Copy a footprint from another board with `pcbnew.FOOTPRINT(src_fp)` and
    keep the source board referenced until done (`Duplicate()` crashed).
  - An uncaught exception segfaults in garbage collection while Ubuntu's
    apport hook runs, hiding the traceback. To see it, run the script under
    `try: exec(...) except: traceback.print_exc()` then `os._exit(0)`.
  - Pads on no net trip schematic parity: give unconnected pads the
    `unconnected-(...)` net names the netlist uses.
- Parts to hand-solder (the user's preference): R/C 0805 hand-solder
  footprints; regulators on the **back**.
- Placement constraints so far: antennas and the microSD slot point off the
  top edge (antenna region overhanging the edge, its keep-out clear).
  - **XL J1 (PBI)**: footprint on F.Cu, connector soldered from the back,
    odd row (pin 1, GND, left) on top. Never flip it to B.Cu: that mirrors the
    holes.
  - **DevKitC carrier**: the PBI connector and the DevKitCs go on opposite
    sides of the board, the DevKitCs facing out from the Atari. There's no
    clearance for them on the PBI connector's side.
- See it: `kicad-cli pcb render -o top.png --side top -w 1600 -h 900 <board>`
  (and `--side bottom`, `--zoom`), then read the PNG.

## Autorouting

```
ROUTE_TIMEOUT=1200 hardware/tools/autoroute.sh <board.kicad_pcb> ~/kicad-check/<run>/<name>-routed.kicad_pcb 60
```

It never touches the input. Work in a scratch directory (`~/kicad-check/`),
look at the DRC summary it prints, render the result, then copy the routed
board over the original and commit. What it does, and why:

1. **Export DSN** through pcbnew (`tools/specctra.py export`). Routing
   boundary inset 0.5 mm (KiCad's edge clearance: Freerouting doesn't know
   it), following the real outline, plus keepouts over the off-board parts
   of its box and a fence around it. Freerouting cuts concave corners (the
   XE's tab notch) and strays a little past straight edges without them.
2. **Route** with Freerouting, headless, pass limit plus `timeout`. It writes
   the session only at the end, so a run the timeout stops yields nothing.
   The optimizer stage is off: on these boards it took 90 s a pass and
   improved nothing. Typical runs take 1.5–6 min.
3. **Route GND too, then pour.** Leaving GND to the pours alone
   (`SKIP_NETS=GND`) left about 20 islands the stitching couldn't join.
   Routing it and then pouring GND on both layers (`specctra.py import
   --pour GND`, with stitching vias where they join islands) leaves nothing
   unconnected.
4. **Clean up**: GND tracks that DRC flags for clearance are deleted
   (`specctra.py cleanup`); the pour carries GND there.
5. **DRC** with the board's `.kicad_pro`/`.kicad_dru` copied alongside.

Expected DRC leftovers (not errors to chase): `drill_out_of_range` ×24 (the
WROOM footprint's 0.2 mm thermal vias), `silk_edge_clearance` (module
silkscreen over the antenna overhang), a few `starved_thermal`,
`lib_footprint_issues` for footprints embedded from libraries not in the
table. Anything in `clearance`, `copper_edge_clearance`, `hole_to_hole`,
`unconnected_items` or schematic parity is real.

If routing doesn't complete, move parts rather than raising passes: the
log's `pass #N ... (K unrouted` lines show whether it's converging.

## Shell hazards

- `pkill -f <pattern>` matches the shell running it when the pattern
  appears in its own command line, and kills your command. Use
  `pgrep -f` to get PIDs and `kill` them, or a pattern that can't match
  itself.

## Rules from the user

- Serial flashing of the main ESP32 (J5 or the Sidecar's UART) only with the
  Atari unplugged; unattended test-farm machines flash main by OTA only.
- Boot-pin hazards (GPIO43 = D5 = U0TXD, GPIO45 = D7 = VDD_SPI strap): see the
  repo-root AGENTS.md.
