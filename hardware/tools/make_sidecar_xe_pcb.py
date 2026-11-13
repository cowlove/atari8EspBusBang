#!/usr/bin/python3
"""Build the sidecar_pcb_xe board from sidecar_pcb_xl's.

Takes the XL board's footprints (their schematic links carry over: the XE
schematic is the XL one with J1 swapped), drops its PBI connector, routes
and outline, and adds the eci2Pbi adapter's cartridge + ECI card edge
(Atari8:CART+ECI_V3.0, copied with its tab outline) as J1, pads on the nets
the XE netlist gives. Then places everything for the card-edge board: tabs
along the bottom edge, modules' antennas and the microSD off the top, body
as wide as the tabs, plus a wing on the right for the optional RCA jack J8.
Unrouted: run autoroute.sh on the result.

    kicad-cli sch export netlist -o /tmp/xe.net hardware/sidecar_pcb_xe/sidecar_pcb_xe.kicad_sch
    /usr/bin/python3 hardware/tools/make_sidecar_xe_pcb.py hardware/sidecar_pcb_xl/sidecar_pcb_xl.kicad_pcb \\
        hardware/eci2Pbi/atari8EciToPbi.kicad_pcb /tmp/xe.net hardware/sidecar_pcb_xe/sidecar_pcb_xe.kicad_pcb

Needs KiCad's pcbnew module (/usr/bin/python3 with the KiCad PPA).
"""
import os
import re
import sys
import uuid

import kicad_netlist
from kicad_compat import pcbnew   # KiCad 9 and 10 (kicad_compat.py)

xl_pcb, eci_pcb, xe_net, dst = sys.argv[1:5]
SCH = os.path.basename(dst).replace(".kicad_pcb", ".kicad_sch")
J1_UUID = str(uuid.uuid5(uuid.NAMESPACE_URL, "atari8EspBusBang/sidecar_pcb_xe/1"))  # make_sidecar_xe_schematic.py
mm = pcbnew.FromMM

board = pcbnew.LoadBoard(xl_pcb)
for t in list(board.GetTracks()):
    board.Delete(t)
for z in list(board.Zones()):
    board.Delete(z)
for d in list(board.GetDrawings()):
    # The outline, and the XL board's texts (J1's back-side note, the date).
    if d.GetLayer() == pcbnew.Edge_Cuts or d.GetClass() == "PCB_TEXT":
        board.Delete(d)
board.Delete(board.FindFootprintByReference("J1"))

# --- J1: the adapter's CART+ECI footprint.
src_board = pcbnew.LoadBoard(eci_pcb)      # keep: the footprint belongs to it
j1 = pcbnew.FOOTPRINT(src_board.FindFootprintByReference("J1"))
board.Add(j1)
j1.SetPath(pcbnew.KIID_PATH("/" + J1_UUID))
pin_net = {}
for name, body in re.findall(r'\(net \(code "\d+"\) \(name "([^"]+)"\)(.*?)\)\s*(?=\(net |\)\s*\)\s*$)',
                             kicad_netlist.read(xe_net), re.S):
    for pin in re.findall(r'\(node \(ref "J1"\) \(pin "([^"]+)"\)', body):
        pin_net[pin] = name
for pad in j1.Pads():
    name = pin_net.get(pad.GetNumber(), "")
    if not name:
        pad.SetNetCode(0)
        continue
    net = board.FindNet(name)
    if net is None:
        net = pcbnew.NETINFO_ITEM(board, name)
        board.Add(net)
    pad.SetNet(net)
for fp in board.GetFootprints():
    fp.SetSheetfile(SCH)

# --- Placement. Body x 100..180.64 (the tabs' width), y 60..100; tabs below.
X0, X1, YTOP, YTAB = 100.0, 180.64, 60.0, 100.0
XR = 196.0    # the right wing's edge, for J8
V = 90
PLACE = {
    # Top row: modules flush with the top edge (antennas overhang it, as on
    # the XL board), microSD between them, card slot up.
    "U1": (110.1, YTOP + 7.62, 0), "J2": (140.32, YTOP + 10.16, 180), "U2": (170.54, YTOP + 7.62, 0),
    # Between U1 and J2: main EN RC and 3V3 decoupling; J5's UART resistors, IO0 pull-up.
    "C3": (122.5, 64.5, V), "R8": (122.5, 68.5, V), "C4": (122.5, 72.5, V),
    "R15": (127.5, 64.5, V), "R16": (127.5, 68.5, V), "R9": (127.5, 72.5, V),
    # Between J2 and U2: SD pull-ups; Sidecar 3V3/EN/CD/BOOT.
    "R2": (152.5, 63.0, V), "R3": (152.5, 67.0, V), "R4": (152.5, 71.0, V),
    "R5": (152.5, 75.0, V), "R6": (152.5, 79.0, V),
    "C7": (157.0, 63.0, V), "R10": (157.0, 67.0, V), "C8": (157.0, 71.0, V),
    "R14": (157.0, 75.0, V), "R11": (157.0, 79.0, V),
    # Bottom-left: main headers and regulator (U3 on the back).
    "J5": (102.5, 84.0, 0), "J6": (110.0, 84.0, 0), "J3": (114.5, 84.0, 0),
    "U3": (106.0, 95.0, 180), "C1": (113.5, 94.0, 0), "C2": (113.5, 97.5, 0),
    "R1": (120.0, 84.0, 0), "R17": (124.5, 84.0, 0), "D2": (129.0, 84.0, 0),
    "C10": (136.0, 84.0, 0),
    # Bottom-right: Sidecar status, programming, breakout, regulator (U4 on the back).
    "R12": (144.0, 84.0, 0), "R13": (148.5, 84.0, 0), "R7": (144.0, 88.0, 0), "D1": (149.5, 89.0, 0),
    "J4": (159.0, 84.0, 0), "U4": (169.0, 86.5, 180),
    "C9": (176.0, 84.0, 0), "C5": (176.0, 87.5, 0), "C6": (176.0, 91.0, 0),
    "J7": (157.5, 97.0, 90),
    # Right wing (past the tabs): the optional NTSC jack J8, pointing right,
    # its front face 1.5 mm inside the edge (its locating-foot cutouts stay
    # closed), low in the wing: U2's courtyard (the antenna keep-out, 48 mm
    # wide) reaches y 81.1. The ladder R18-R21 in a row between them.
    "J8": (XR - 1.5, 93.0, 180),
    "R18": (181.0, 83.8, V), "R19": (184.8, 83.8, V), "R20": (188.6, 83.8, V), "R21": (192.4, 83.8, V),
}
for ref, (x, y, r) in PLACE.items():
    fp = board.FindFootprintByReference(ref)
    fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    fp.SetOrientationDegrees(r)
missing = {fp.GetReference() for fp in board.GetFootprints()} - set(PLACE) - {"J1"}
assert not missing, missing

# J1 rotated 180: tabs hang below the body, fingers at the bottom edge. Its
# origin is the tabs' outer corner (local 0.016, -0.055 from the outline).
j1.SetOrientationDegrees(180)
j1.SetPosition(pcbnew.VECTOR2I(mm(X1 + 0.016), mm(YTAB - 0.055)))

# --- Outline: the footprint's tab outline is open at its two outer corners;
# close it around the body, with the right wing (J8) out to XR.
ends = {}
for g in j1.GraphicalItems():
    if g.GetLayer() == pcbnew.Edge_Cuts:
        for p in (g.GetStart(), g.GetEnd()):
            k = (p.x, p.y)
            ends[k] = ends.get(k, 0) + 1
open_ends = sorted(k for k, n in ends.items() if n == 1)
assert len(open_ends) == 2, open_ends
(ax, ay), (bx, by) = open_ends
top = mm(YTOP)
xr = mm(XR)
for p, q in (((ax, ay), (ax, top)), ((ax, top), (xr, top)), ((xr, top), (xr, by)), ((xr, by), (bx, by))):
    seg = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
    seg.SetLayer(pcbnew.Edge_Cuts)
    seg.SetWidth(mm(0.1))
    seg.SetStart(pcbnew.VECTOR2I(*p))
    seg.SetEnd(pcbnew.VECTOR2I(*q))
    board.Add(seg)

pcbnew.SaveBoard(dst, board)
bb = board.GetBoardEdgesBoundingBox()
print(f"outline {pcbnew.ToMM(bb.GetWidth()):.2f} x {pcbnew.ToMM(bb.GetHeight()):.2f} mm; "
      f"J1 pads on nets: {sum(1 for p in j1.Pads() if p.GetNetCode())} of {len(list(j1.Pads()))}")
