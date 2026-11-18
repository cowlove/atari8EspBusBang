# Ordering the Sidecar boards from PCBWay

How to fill in PCBWay's order forms for the two bare-module Sidecar boards,
and the answers to the questions PCBWay's engineers asked when they reviewed
the first order. The files here come from `../../tools/export_pcbway.sh`.

| Board | Files | State |
|---|---|---|
| `sidecar_pcb_xl` (PBI) | `sidecar_pcb_xl.zip`, `-bom.csv`, `-cpl.csv` | **The first order (2026-11-13), 5 assembled.** Kept as ordered: the board files have moved on (USB-C and Sidecar RESET in the schematic, not yet laid out), so `export_pcbway.sh` no longer regenerates these. |
| `sidecar_pcb_xe` (cartridge + ECI) | `sidecar_pcb_xe.zip`, `-bom.csv`, `-cpl.csv` | The current revision, with the Sidecar's USB-C (J9) and RESET drive (R22). Not ordered yet. |

PCBWay's forms change from time to time; these were right in November 2026.
Sign in before you start, so the order stays with your account.

## 1. PCB (Quick-order PCB)

Upload the board's zip first: it fills in the size and layer count, which
should match the table.

| Field | XL | XE |
|---|---|---|
| Board type | Single pieces | Single pieces |
| Different designs in panel | 1 | 1 |
| Size (auto-filled) | 124.55 × 35.61 mm | 96.11 × 60.11 mm |
| Quantity | your choice; 5 is the usual minimum | same |
| Layers | 2 | 2 |
| Material | FR-4, TG130–150 | FR-4, TG130–150 |
| Thickness | 1.6 mm | 1.6 mm (a cartridge edge needs it) |
| Min track/spacing | 6/6 mil (the boards use 0.2 mm, about 8 mil) | same |
| Min hole size | 0.3 mm | 0.3 mm |
| Solder mask / silkscreen | your choice; green and white are cheapest | same |
| Edge connector | No | **Yes**: gold fingers, 30° or 45° chamfer |
| Surface finish | HASL lead-free | to suit the gold fingers (hard gold on the fingers lasts through repeated insertions; HASL works but wears) |
| Via process | Tenting vias | Tenting vias |
| Finished copper | 1 oz | 1 oz |
| Remove product No. | your choice (looks only; the silkscreen has a `REV` stamp) | same |
| Castellated holes / impedance control | No | No |

Both boards have J8's plated slots (the optional RCA jack) in the PTH drill
file, and two small internal cutouts on the outline. PCBWay normally finds
them in review; if there's a notes box on this page, say so.

## 2. Assembly (the "other parameters" page)

| Field | XL | XE |
|---|---|---|
| Service | Turnkey (PCBWay sources the parts) | same |
| Accept substitutes | Yes, for resistors and capacitors | same |
| Board type | Single pieces | Single pieces |
| Assembly quantity | your choice (can be fewer than the PCBs) | same |
| Sides | Both sides (U3 and U4 are on the bottom) | Both sides |
| Unique parts (BOM lines) | 18 | 20 |
| SMD parts (placements) | 34 | 39 (J9 included: SMD pins plus through-hole shell tabs) |
| BGA/QFP parts | 0 (QFN/LGA: 2, if there's a separate field, for the two modules) | same |
| Through-hole parts | 6 (J1, J3, J4, J5, J6, J7) | 5 (J3, J4, J5, J6, J7) |
| Sensitive components | Yes: the ESP32-S3-WROOM-1 modules and the SK6812MINI are moisture-sensitive (bake before reflow) | same |
| Conformal coating, X-ray, firmware loading, function test | No | No |
| Packaging | antistatic is fine | same |

The counts leave out the do-not-populate NTSC parts (R18–R21 and J8): they
aren't in the BOM or placement files. The ESP32 modules are castellated with
a bottom ground pad, not BGA or QFP.

**Notes box, ready to paste:**

- XL:
  > Parts on both sides: U3, U4 (AP7361C, SOT-223) on the bottom. J1 (TE 5530843-5, PBI card edge connector) is inserted from the BOTTOM side and soldered on the top, although its footprint is on the top layer. R18-R21 and J8 are DNP (not in BOM/CPL). Two ESP32-S3-WROOM-1 modules (MSL 3). J8 plated slots are in the PTH drill file. D1 is an OPSCO SK6812MINI (3.5x3.5 mm PLCC4, top-emitting, not -E): pin 1 DOUT, 2 GND, 3 DIN, 4 VDD; DIN is pad 3, at the cut corner of the D1 silkscreen outline.
- XE:
  > Parts on both sides: U3, U4 (AP7361C, SOT-223) on the bottom. The bottom tabs are cartridge/ECI edge fingers: keep them free of solder and parts. R18-R21 and J8 are DNP (not in BOM/CPL). Two ESP32-S3-WROOM-1 modules (MSL 3). J9 (GCT USB4105-GF-A, USB-C) sits with its mating face flush with the board edge; its shell tabs are through-hole. D1 is an OPSCO SK6812MINI (3.5x3.5 mm PLCC4, top-emitting, not -E): pin 1 DOUT, 2 GND, 3 DIN, 4 VDD; DIN is pad 3, at the cut corner of the D1 silkscreen outline.

## 3. Upload page (in the cart, after "Add to cart")

Each board's files in their own slots:

| Slot | XL | XE |
|---|---|---|
| Gerber | `sidecar_pcb_xl.zip` | `sidecar_pcb_xe.zip` |
| BOM | `sidecar_pcb_xl-bom.csv` | `sidecar_pcb_xe-bom.csv` |
| Centroid / pick-and-place | `sidecar_pcb_xl-cpl.csv` | `sidecar_pcb_xe-cpl.csv` |

PCBWay's engineers review the order before it's confirmed, usually within a
few hours to a day.

## 4. PCBWay's review questions (first XL order) and our answers

PCBWay sent these as a spreadsheet (the BOM with a "PCBWay Note" column, our
answers going in "Customer Reply"). They apply to the XE board as well.

| Part | PCBWay asked | Answer |
|---|---|---|
| D1, SK6812MINI | Several suffixes exist: confirm the full model number. Moisture-sensitive: needs baking. Which pad is DIN? | OPSCO **SK6812MINI**, the standard top-emitting 3.5 × 3.5 mm PLCC4 (footprint `LED_SK6812MINI_PLCC4_3.5x3.5mm_P1.75mm`), **not SK6812MINI-E** (reverse mount, a different body). Pinout per the OPSCO datasheet: 1 DOUT, 2 GND, 3 DIN, 4 VDD. On the board, DIN is **pad 3, at the cut (chamfered) corner of the D1 silkscreen outline**; diagonally opposite is pad 1, DOUT, unconnected. GND is pad 2, on DOUT's side; VDD (3.3 V) is pad 4. Orient the LED so its pin 3 is on that pad. Baking before reflow is fine. (An SK6812MINI-HS substitute only if its datasheet pinout is the same.) |
| J3, J4, J5, J6, J7 (pin headers) | Proposed CJT A2541WV-3P, -2x3P, -2P, -2x7P: please confirm | Confirmed, as long as they are 2.54 mm pitch, vertical (straight), male, standard-length pins: J3 1×3, J4 and J5 2×3, J6 1×2, J7 2×7. (J7 pins 9–11 are unconnected on the board; the 2×7 header is still right.) |
| J1 (XL only), TE 5530843-5 | (Covered by the notes box) | Inserted from the bottom side, soldered on the top; the footprint is on the top layer. |

The rest of the sheet was prices: for the XL board, components $107.29 for
5 boards ($21.46 each), before assembly and PCB costs.

### Second round: the "Quotation" sheet (2026-11-17)

After our reply, PCBWay sent the original question sheet again, three times
(byte-identical files: no new questions), plus a sheet named
`Quotation T-M8W835129A-…(2026-11-17).xls` in the same layout. That one is new:
compare the files (`sha256sum`) before assuming a resend is a copy, and read
the PCBWay Note and Actual Purchase Mfg Part # columns for what's changed.

- **What they accepted:** the CJT A2541WV headers (no more "please confirm"),
  and a priced D1: $1.53 each, 7–10 workdays.
- **What they asked again:** D1 only, "[Sensitive components]: bake before
  placement. Which pad on the board does the DIN mark correspond to?" (the
  note is part Chinese, 【敏感元器件】：需要除湿的 贴片前烘烤 = sensitive
  component, needs dehumidifying, bake before placement).
- **The answer, short enough for the Customer Reply cell:**
  > DIN is pad 3: bottom-right in top view, at the chamfered (cut) corner of the D1 silkscreen outline; net SC_LED, to U2 IO48. Pad 1 (DOUT, not connected) is diagonally opposite, top-left. Pad 2 = GND (bottom-left), pad 4 = VDD 3.3 V (top-right). Pinout per the OPSCO SK6812MINI datasheet: 1 DOUT, 2 GND, 3 DIN, 4 VDD. Pre-reflow baking OK.

  Checked against `sidecar_pcb_xl.kicad_pcb` (D1 unrotated, top side): the
  silkscreen's chamfer and the fab layer's corner mark are both at pad 3, not
  pad 1. Don't tell them "pin 1 is the marked corner" for this footprint.
- **The quoted total for 5 assembled XL boards:** components $114.94,
  assembly $29.00, PCB $26.23, less the 1% member discount ($0.55):
  **$169.62, $33.92 per board.**
