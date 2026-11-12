#!/usr/bin/python3
"""Place the espBusBangSidecar (DevKitC carrier) board from scratch.

Runs on a board netlist_to_pcb.py has brought up to date: deletes all
tracks and board texts, widens the outline for U2 and the header strip, and
places every part but J1, U1 and R1 (kept where espBusBang has them).
J1 (PBI) mounts on the back, the DevKitCs on the front, facing out from the
Atari: there is no room for them on J1's side. Then autoroute.sh.

    /usr/bin/python3 hardware/tools/place_sidecar_devkitc.py <board.kicad_pcb>
"""
import os
import sys

import pcbnew

path = sys.argv[1]
b = pcbnew.LoadBoard(path)
for t in list(b.GetTracks()):
    b.Delete(t)
for d in list(b.GetDrawings()):
    if d.GetClass() == "PCB_TEXT":
        b.Delete(d)
mm = pcbnew.FromMM
X0, X1 = 92.5, 163.8   # X1: U1's outer pads 0.5 mm in (the original has 0.38)
for d in b.GetDrawings():
    if d.GetLayer() == pcbnew.Edge_Cuts:
        s, e = d.GetStart(), d.GetEnd(); d.SetStart(pcbnew.VECTOR2I(mm(X0), s.y)); d.SetEnd(pcbnew.VECTOR2I(mm(X1), e.y))
U2X = 137.1637 - 28.6
P = {
    "U2": (U2X, 45.7227, 0),
    # header strip beyond U2: column A (2-row headers), column B (1-row)
    "J7": (95.0, 47.0, 0), "J4": (95.0, 69.0, 0), "J5": (95.0, 78.0, 0),
    "R15": (96.3, 87.5, 0), "R16": (96.3, 90.5, 0),
    "J2": (103.0, 47.0, 0), "J3": (103.0, 69.0, 0),
    "J8": (103.0, 78.0, 0), "J9": (103.0, 84.5, 0), "J10": (103.0, 91.0, 0),
    "D2": (96.3, 100.0, 0), "R17": (96.3, 103.5, 0),
    # under U2, between its header rows: SD pull-ups, audio, flash UART
    "R2": (116.0, 52.0, 90), "R3": (116.0, 57.0, 90), "R4": (116.0, 62.0, 90),
    "R5": (116.0, 67.0, 90), "R6": (116.0, 72.0, 90),
    "R12": (126.0, 80.0, 90), "R13": (126.0, 85.0, 90), "R7": (126.0, 92.0, 90),
}
for ref, (x, y, r) in P.items():
    fp = b.FindFootprintByReference(ref)
    fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    fp.SetOrientationDegrees(r)
pcbnew.SaveBoard(path, b)
bb = b.GetBoardEdgesBoundingBox()
print("outline", pcbnew.ToMM(bb.GetOrigin()), pcbnew.ToMM(bb.GetEnd())); sys.stdout.flush()
os._exit(0)
