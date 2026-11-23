#!/usr/bin/env python3
"""Generate the sidecar_pcb_hdr schematic: one Sidecar board for every Atari, on the 2x25 bus header.

Starts, as make_sidecar_xe_schematic.py does, from a sidecar_pcb_xl
schematic built with --sidecar-usbc (so it has the XE board's Sidecar USB-C
and RESET drive) and its netlist, and swaps the PBI connector J1 for the
bus header socket (bus_header.py): header pin n carries the XL board's net
for PBI pin n, the PBI's NC pins are GND. A per-machine adapter
(bus_adapter_pbi, bus_adapter_eci) plugs into it. Everything but J1 is
the XL (= XE) circuit, UUIDs included, so the XE board's footprints keep
their links.

    kicad-cli sch export netlist -o /tmp/sc.net hardware/espBusBangSidecar/espBusBangSidecar.kicad_sch
    python3 hardware/tools/make_sidecar_xl_schematic.py hardware/espBusBangSidecar/espBusBangSidecar.kicad_sch \\
        /tmp/sc.net hardware/symbols /tmp/xl-usbc.kicad_sch --sidecar-usbc
    kicad-cli sch export netlist -o /tmp/xl-usbc.net /tmp/xl-usbc.kicad_sch
    python3 hardware/tools/make_sidecar_hdr_schematic.py /tmp/xl-usbc.kicad_sch /tmp/xl-usbc.net \\
        hardware/symbols hardware/sidecar_pcb_hdr/sidecar_pcb_hdr.kicad_sch

Deterministic UUIDs: reruns only show changes. Prints the pin map.
"""
import re
import sys

import bus_header
import kicad_netlist
import sch_gen as g

xl_sch, xl_net, symdir, dst = sys.argv[1:5]
s = open(xl_sch).read()
U = g.Uuids("atari8EspBusBang/sidecar_pcb_hdr")
xl_pbi = g.pin_nets(kicad_netlist.read(xl_net), "J1")       # XL board: PBI pin -> net

# --- Drop the PBI connector J1 and the labels on its pins (as the XE generator does).
CONN = "CONN50_5530843-5_TEC:5530843-5"
m = re.search(r'\n\t\(symbol\n\t\t\(lib_id "' + re.escape(CONN) + r'"\)\n\t\t\(at ([-\d.]+) ([-\d.]+) 0\)', s)
jx, jy = float(m.group(1)), float(m.group(2))
start = m.start() + 1
s = s[:start] + s[start + len(g.block(s, start)):]
lib = g.block(s, s.index(f'(symbol "{CONN}"'))
spots = {(round(jx + p[0], 2), round(jy - p[1], 2)) for p in g.pins(lib).values()}
dropped = []


def drop_label(m):
    if (round(float(m.group(2)), 2), round(float(m.group(3)), 2)) in spots:
        dropped.append(m.group(1))
        return ""
    return m.group(0)


s = re.sub(r'\t\(label "([^"]*)"\n\t\t\(at ([-\d.]+) ([-\d.]+) \d+\)\n(?:\t\t[^\n]*\n)*?\t\)\n', drop_label, s)
# A wire from a part to a J1 pin (R1 to RESET): drop it, label its far end with the pin's net.
label_at = {(round(float(x), 2), round(float(y), 2)): name for name, x, y in
            re.findall(r'\(label "([^"]*)"\n\t\t\(at ([-\d.]+) ([-\d.]+) \d+\)', open(xl_sch).read())}
wire_labels = []
for w in list(re.finditer(r'\t\(wire\n\t\t\(pts\n\t\t\t\(xy ([-\d.]+) ([-\d.]+)\) \(xy ([-\d.]+) ([-\d.]+)\)\n'
                          r'(?:\t\t[^\n]*\n)*?\t\)\n', s)):
    a = (round(float(w.group(1)), 2), round(float(w.group(2)), 2))
    b = (round(float(w.group(3)), 2), round(float(w.group(4)), 2))
    for pin_end, far in ((a, b), (b, a)):
        if pin_end in spots:
            s = s.replace(w.group(0), "", 1)
            wire_labels.append((label_at[pin_end], far))
k = s.rindex('\n', 0, s.index(f'(symbol "{CONN}"')) + 1
s = s[:k] + s[k + len(lib) + s[k:].index('(') + 1:]

# --- The socket: Conn_02x25_Odd_Even from the vendored library.
hdr_lib = g.lib_from_symdir(symdir, "Connector_Generic", "Conn_02x25_Odd_Even")
hdr_pins = g.pins(hdr_lib)
end = s.index('\t(lib_symbols')
end = end + 1 + len(g.block(s, end + 1)) - 1
s = s[:end].rstrip('\t') + "\t\t" + hdr_lib + "\n\t" + s[end:]

nets = {}
for n, sig in bus_header.PINS.items():
    nets[str(n)] = "GND" if sig == "GND" else xl_pbi[str(n)].lstrip("/")
    assert sig == "GND" or nets[str(n)].startswith(sig), (n, sig, nets[str(n)])   # VCC50 -> VCC50_1/_2
ROOT = re.search(r'\(uuid "([^"]+)"\)', s).group(1)
# Where J1 was: the socket's odd column (x -5.08) on J1's pin column, its top pin on J1's first.
cx, cy = jx + 5.08, jy + 30.48
out = [g.symbol(U, ROOT, "Connector_Generic:Conn_02x25_Odd_Even", "J1", "Bus socket 2x25", bus_header.SOCKET_FP,
                "Bus header socket (hardware/tools/bus_header.py): takes an Atari adapter", cx, cy,
                sorted(hdr_pins, key=int))]
out += g.pin_labels(U, cx, cy, hdr_pins, nets)
for name, (x, y) in wire_labels:
    out.append(g.label(U, name, x, y, 180))
s = s.replace("J6: PBI 5V (VCC50_1)", "J6: Atari 5V (VCC50_1, bus header 47)")
NOTE = ("sidecar_pcb_hdr: sidecar_pcb_xl/xe with J1 a 2x25 bus header socket; an adapter per machine "
        "(bus_adapter_pbi, bus_adapter_eci) plugs in. Header pin n = PBI pin n (hardware/tools/bus_header.py); "
        "the PBI's NC pins 33, 37, 39 are GND.")
out.append(g.text(U, NOTE, jx - 20, cy + 85))
k = s.index('\t(sheet_instances')
s = s[:k] + "".join(out) + s[k:]
open(dst, "w").write(s)
print(f"dropped J1 ({len(spots)} pins, {len(dropped)} labels, {len(wire_labels)} wires); socket at {cx:g},{cy:g}")
for n in range(1, 51):
    print(f"  {n:>2} {nets[str(n)]}", end="\n" if n % 5 == 0 else "")
