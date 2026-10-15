# Hardware

This directory contains the KiCad designs for the Atari 8-bit ESP bus
interface and its optional ECI-to-PBI adapter.

## Boards

- **ESP bus interface:** [`espBusBang/`](espBusBang/) — board for connecting the esp32 to the PBI bus.
- **ECI-to-PBI adapter:** [`eci2Pbi/`](eci2Pbi/) — optional adapter board for XE systems.

## ESP32 pinout

This project is designed for the exact board and pinout shown in the
[ESP32-S3-DevKitC-1-N8R2 reference image](ESP32-S3-N8R2-board.jpg). The
specific ESP32-S3-N8R2 module and header pin assignments must all match; boards with similar pinouts and similar-looking
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

## Assembly instructions

You have two chances to solder a header on the wrong side, and one chance to insert the ESP32 board backwards.  If you want 95% certainty of assembling one right, order 23 sets. 

## Notes

Open the `.kicad_pro` file in KiCad to inspect or edit a design. For fabrication,
upload the corresponding `.kicad_pcb` file to a board house. 
