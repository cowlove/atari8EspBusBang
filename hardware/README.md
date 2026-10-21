# Hardware

This directory contains the KiCad designs for the Atari 8-bit ESP bus
interface and its optional ECI-to-PBI adapter.

## Boards

- **ESP bus interface:** [`espBusBang/`](espBusBang/) — board for connecting the esp32 to the PBI bus.
- **ECI-to-PBI adapter:** [`eci2Pbi/`](eci2Pbi/) — optional adapter board for XE systems.

## ESP32 pinout

This project is designed for the exact board and pinout shown in the
[ESP32-S3-DevKitC-1-N8R2 reference image](ESP32-S3-N8R2-board.jpg). The
specific ESP32-S3-N8R2 module and header pin assignments must all match; boards with similar pinouts but with 
ESP32-S3 modules not ending in "R2" are not interchangeable.

## Bill of materials

The ESP bus interface requires one ESP32-S3 development board, one Sullins
connector, and the espBusBang PCB. The ECI-to-PBI adapter PCB is optional.

| Qty. | Item | Source / notes |
| ---: | --- | --- |
| 1 | ESP32-S3-DevKitC-1-N8R2 | **The exact model matters:** only the ESP32-S3-DevKitC-1-N8R2 with the N8R2 module is suitable; similar-looking ESP32-S3 variants are not interchangeable. Not currently available from DigiKey. [Example Amazon listing](https://www.amazon.com/dp/B0C9H7Y66W?ref=ppx_yo2ov_dt_b_fed_asin_title&th=1) (about $6 each when available; listings change and may be counterfeit, though reported units have worked). Verify the exact model and N8R2 module marking before ordering. |
| 1 | Sullins EBC25DCWN connector | [DigiKey](https://www.digikey.com/en/products/detail/sullins-connector-solutions/EBC25DCWN/927272) |
| 1 order | espBusBang PCB | Upload [`espBusBang/atariEspBusBang3.kicad_pcb`](espBusBang/atariEspBusBang3.kicad_pcb) directly to [OSH Park](https://oshpark.com/). The minimum 3-board order is about $14. |
| Optional, 1 order | ECI-to-PBI adapter PCB | Upload [`eci2Pbi/atari8EciToPbi.kicad_pcb`](eci2Pbi/atari8EciToPbi.kicad_pcb) directly to [OSH Park](https://oshpark.com/). A 3-board order is about $22. |

## PCBWay Gerber export

For PCBWay orders, run `./hardware/tools/export_pcbway.sh` from the repository
root. It uses `kicad-cli` (KiCad 9 or newer) to export each board into its own
directory and ZIP under `hardware/gerbers/pcbway/`. The generated directory
is replaced on each successful run. The two ZIPs are checked into Git for
convenient ordering; regenerate them with this script whenever either board
changes so they stay in sync with the KiCad source. Upload only the appropriate
board's ZIP to PCBWay, and do not mix files from the two boards.

Each ZIP contains the front and back copper, solder mask, silkscreen, board
outline, and Excellon drill files split into plated (`PTH`) and non-plated
(`NPTH`) holes. Gerbers use standard RS-274X (X2 and embedded netlist attributes
disabled); drills use millimeters and the absolute board origin. PCBWay's
online Gerber viewer should be used to verify layer assignment, board outline,
and hole placement before ordering. Run KiCad's DRC separately before sending
an order; this export script does not certify the design.

Install KiCad 9+ and `zip` first. PCBWay's requirements can change; see its
[KiCad export guide](https://www.pcbway.com/helpcenter/generate_gerber/How_to_Export_Gerber__BOM__and_Pick_and_Place_Files_in_KiCad_9_0.html)
and [Gerber extension reference](https://www.pcbway.com/helpcenter/generate_gerber/Gerber_File_Extension_from_Different_Software.html)
for current guidance.

## Assembly instructions

You have two chances to solder a header on the wrong side, and one chance to insert the ESP32 board backwards.  If you want 95% certainty of assembling one right, order 23 sets. 

## Notes

Open the `.kicad_pro` file in KiCad to inspect or edit a design. For fabrication,
upload the corresponding `.kicad_pcb` file to a board house that accepts KiCad
design files, or export Gerber manufacturing files as described above.
