#!/usr/bin/env python3
"""Generate the two bus adapter schematics: an Atari's connector to the 2x25 bus header.

    bus_adapter_pbi: J1 the PBI card-edge socket (600XL/800XL), J2 the header.
        A straight pass-through (bus_header.py: header pin n = PBI pin n);
        the PBI's NC pins stay open, the header's matching pins are GND.
    bus_adapter_eci: J1 the cartridge + ECI card edge (130XE/65XE), J2 the
        header. The eci2Pbi adapter's mapping: each card-edge pin goes to
        the header pin of the PBI signal eci2Pbi wires it to (ECI C1, HALT,
        to READY, as there; see the memory note on the XE's HALT). EXTDEC,
        CAS and RAS have no ECI pin: open on the header.

    kicad-cli sch export netlist -o /tmp/eci.net hardware/eci2Pbi/atari8EciToPbi.kicad_sch
    python3 hardware/tools/make_bus_adapters.py hardware/symbols \\
        hardware/sidecar_pcb_xl/sidecar_pcb_xl.kicad_sch \\
        hardware/eci2Pbi/atari8EciToPbi.kicad_sch /tmp/eci.net hardware

The symbols: the PBI socket from the XL schematic's embedded library, the
card edge from eci2Pbi's, Conn_02x25_Odd_Even from hardware/symbols (KiCad
9.0.7). Deterministic UUIDs: reruns only show changes. Prints both maps.
"""
import os
import sys

import bus_header
import kicad_netlist
import sch_gen as g

symdir, xl_sch, eci_sch, eci_net, outdir = sys.argv[1:6]

HDR_LIB = g.lib_from_symdir(symdir, "Connector_Generic", "Conn_02x25_Odd_Even")
HDR_PINS = g.pins(HDR_LIB)
HDR_ID = "Connector_Generic:Conn_02x25_Odd_Even"
PBI_ID = "CONN50_5530843-5_TEC:5530843-5"
ECI_ID = "Atari8:CART+ECI"


def header(U, root, x, y, nets, items):
    items.append(g.symbol(U, root, HDR_ID, "J2", "Bus header 2x25", bus_header.HEADER_FP,
                          "Bus header (hardware/tools/bus_header.py): mates the board's J1 socket",
                          x, y, sorted(HDR_PINS, key=int)))
    items += g.pin_labels(U, x, y, HDR_PINS, nets)


def write(name, root, libs, items):
    path = os.path.join(outdir, name, name + ".kicad_sch")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w").write(g.new_schematic(root, libs, items))
    print("wrote", path)


hdr_nets = {str(n): s for n, s in bus_header.PINS.items()}

# --- PBI adapter.
U = g.Uuids("atari8EspBusBang/bus_adapter_pbi")
root = U()
pbi_lib = g.lib_from_sch(xl_sch, PBI_ID)
pbi_pins = g.pins(pbi_lib)
items = []
JX, JY = 63.5, 40.64
items.append(g.symbol(U, root, PBI_ID, "J1", "5530843-5", "footprints:CONN50_5530843-5_TEC",
                      "PBI card-edge socket, 50 way (TE 5530843-5)", JX, JY, sorted(pbi_pins, key=int)))
pbi_nets = {p: (None if int(p) in bus_header.PBI_NC else bus_header.PINS[int(p)]) for p in pbi_pins}
items += g.pin_labels(U, JX, JY, pbi_pins, pbi_nets)
header(U, root, 165.1, 101.6, hdr_nets, items)
items.append(g.text(U, "bus_adapter_pbi: the 600XL/800XL PBI to the 2x25 bus header (hardware/tools/bus_header.py). "
                    "Header pin n = PBI pin n; the PBI's NC pins (33, 37, 39) stay open, the header's are GND.",
                    25.4, 190.5))
write("bus_adapter_pbi", root, [pbi_lib, HDR_LIB], items)

# --- ECI adapter.
U = g.Uuids("atari8EspBusBang/bus_adapter_eci")
root = U()
eci_lib = g.lib_from_sch(eci_sch, ECI_ID)
eci_pins = g.pins(eci_lib)
signals = set(bus_header.PINS.values())
eci_nets = {}
for pin, net in g.pin_nets(kicad_netlist.read(eci_net), "J1").items():
    name = net.lstrip("/")
    eci_nets[pin] = name if name in signals else None    # RESERVED, the cartridge-only pins
missing = set(eci_pins) - set(eci_nets)
assert not missing, missing
items = []
JX, JY = 88.9, 40.64
items.append(g.symbol(U, root, ECI_ID, "J1", "CART+ECI", "Atari8:CART+ECI_V3.0",
                      "XE cartridge + ECI card edge", JX, JY, sorted(eci_pins)))
items += g.pin_labels(U, JX, JY, eci_pins, eci_nets)
header(U, root, 165.1, 101.6,
       {p: (None if s in bus_header.NOT_ON_ECI else s) for p, s in hdr_nets.items()}, items)
items.append(g.text(U, "bus_adapter_eci: the 130XE/65XE cartridge + ECI slots to the 2x25 bus header "
                    "(hardware/tools/bus_header.py), with the eci2Pbi adapter's mapping. ECI C1 (HALT from ANTIC) "
                    "= READY, as on eci2Pbi: it halts the 6502 but isn't RDY (bank swaps need the RDY mod). "
                    "No ECI pin for EXTDEC, CAS or RAS: open on the header.", 25.4, 190.5))
write("bus_adapter_eci", root, [eci_lib, HDR_LIB], items)

on_eci = {s for s in eci_nets.values() if s}
print("ECI adapter:", " ".join(f"{p}={eci_nets[p] or '-'}" for p in sorted(eci_nets)))
print("header signals with no ECI pin:", ", ".join(sorted(signals - on_eci - {"GND"})))
