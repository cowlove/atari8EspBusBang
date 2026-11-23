# bus_adapter_eci: 130XE/65XE cartridge + ECI to the bus header (draft)

Plugs into the XE's cartridge and ECI slots and carries the 2x25 bus header
that [sidecar_pcb_hdr](../sidecar_pcb_hdr/) mates with (pinout there and in
`../tools/bus_header.py`). It uses the [eci2Pbi](../eci2Pbi/) adapter's
mapping: each card-edge pin goes to the header pin of the PBI signal that
eci2Pbi wires it to. A generator check confirmed that every header pin has
the same card-edge pins as eci2Pbi's PBI pin of the same number, except
33/37/39, which are GND here.

- **ECI C1 (HALT) goes to READY,** as on eci2Pbi. On the XE it is ANTIC's
  HALT output, not RDY: it stops the 6502, but it can't be used for bank
  swaps without the RDY mod. The trace stays, as on eci2Pbi and
  sidecar_pcb_xe.
- **Left open:** header pins 38 (EXTDEC), 41 (CAS) and 44 (RAS) have no ECI
  pin. On the edge, the cartridge-only pins (S4, S5, RD4, RD5, CCTL) and ECI
  D1XX and RESERVED are open, as on eci2Pbi.
- **Both 5 V pins** (cartridge 13, ECI 1-7) go to header 47 and 48.

| Ref | Part | |
|---|---|---|
| J1 | cartridge + ECI card edge (eci2Pbi's `Atari8:CART+ECI_V3.0`, with its tab outline) | turned 180° as on sidecar_pcb_xe, tabs down; the fingers' "TOP" silkscreen is on the front |
| J2 | 2x25 2.54 mm pin header, vertical, through-hole | front face, centred on the body, with the same rotation as on the PBI adapter (one socket orientation mates both) |

Board 80.8 x 42 mm with the tabs, two layers, autorouted, not reviewed. The
body (22 mm above the tabs) is roomy so the router has space. Its fit in
the XE's cartridge opening, and which way the stack faces there, are
unchecked.

The schematic and board come from the PBI adapter's scripts: see
[bus_adapter_pbi](../bus_adapter_pbi/README.md) for the commands.
`bus_adapter_eci.kicad_dru` lets J1's fingers run to the board edge.
