# Hardware

This directory contains the KiCad designs for the Atari 8-bit ESP bus
interface and its optional ECI-to-PBI adapter.

## Boards

- **ESP bus interface:** [`espBusBang/`](espBusBang/) — KiCad project and PCB
  used by the espBusBang hardware.
- **ECI-to-PBI adapter:** [`eci2Pbi/`](eci2Pbi/) — optional adapter board for
  systems that need to connect an ECI device to the PBI.

Each board directory contains its KiCad schematic (`.kicad_sch`), PCB layout
(`.kicad_pcb`), and project settings (`.kicad_pro`).

## Bill of materials

The ESP bus interface requires one ESP32-S3 development board, one Sullins
connector, and the espBusBang PCB. The ECI-to-PBI adapter PCB is optional.

| Qty. | Item | Source / notes |
| ---: | --- | --- |
| 1 | ESP32-S3-DevKitC-1-N8R2 | **The exact model matters:** only the ESP32-S3-DevKitC-1-N8R2 with the N8R2 module is suitable; similar-looking ESP32-S3 variants are not interchangeable. Not currently available from DigiKey. [Example Amazon listing](https://www.amazon.com/dp/B0C9H7Y66W?ref=ppx_yo2ov_dt_b_fed_asin_title&th=1) (about $6 each when available; listings change and may be counterfeit, though reported units have worked). Verify the exact model and N8R2 module marking before ordering. |
| 1 | Sullins EBC25DCWN connector | [DigiKey](https://www.digikey.com/en/products/detail/sullins-connector-solutions/EBC25DCWN/927272) |
| 1 order | espBusBang PCB | Upload [`espBusBang/atariEspBusBang3.kicad_pcb`](espBusBang/atariEspBusBang3.kicad_pcb) directly to [OSH Park](https://oshpark.com/). A 3-board set is about $14. |
| Optional, 1 order | ECI-to-PBI adapter PCB | Upload [`eci2Pbi/atari8EciToPbi.kicad_pcb`](eci2Pbi/atari8EciToPbi.kicad_pcb) directly to [OSH Park](https://oshpark.com/). A 3-board set is about $22. |

Prices and availability are approximate and can change. The PCB orders are
panelized as three copies by the service; only one board is needed for a single
build.

## Assembly warning

There are two opportunities to solder a component with its orientation
reversed, and one opportunity to insert the ESP32-S3 development board
backwards. Inserting it backwards can destroy the ESP32. Verify each part's
orientation against the schematic and PCB markings before soldering, and check
the development-board orientation again before inserting or applying power.

Assembly yield is not guaranteed. Based on the expected failure rate, order 23
boards if you want about a 95% probability of eventually getting a successful
one. OSH Park supplies boards in sets of three, so that means ordering eight
sets (24 boards).

## Notes

Open the `.kicad_pro` file in KiCad to inspect or edit a design. For fabrication,
upload the corresponding `.kicad_pcb` file to the board house. Review the PCB
and schematic before ordering; this page is a starter parts/order guide, not a
substitute for checking the current design files and assembly requirements.
