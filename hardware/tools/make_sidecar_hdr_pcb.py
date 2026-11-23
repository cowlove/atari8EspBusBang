#!/usr/bin/python3
"""Build the sidecar_pcb_hdr board from sidecar_pcb_xe's (unrouted).

The XE board's placement, with its card edge J1 and tabs gone: J1 is now
the 2x25 bus header socket (bus_header.py), SMD on the back (the face
toward the Atari; the modules face away, as on the XL board), along a
strip added below the XE board's body. Its pads are checked against the
adapters' header (make_bus_adapter_pcbs.py): seen from the front, socket pin n
lies over header pin n.

    kicad-cli sch export netlist -o /tmp/hdr.net hardware/sidecar_pcb_hdr/sidecar_pcb_hdr.kicad_sch
    /usr/bin/python3 hardware/tools/make_sidecar_hdr_pcb.py hardware/sidecar_pcb_xe/sidecar_pcb_xe.kicad_pcb \\
        /tmp/hdr.net hardware/bus_adapter_pbi/bus_adapter_pbi.kicad_pcb hardware/sidecar_pcb_hdr/sidecar_pcb_hdr.kicad_pcb

The netlist also brings the 2026-11-18 re-spin's main pins, which the XE
board never had (J3: RESET on main IO19; GPIO18 link RX; GPIO21 REFRESH):
reroute everything (autoroute.sh).
"""
import os
import subprocess
import sys

from kicad_compat import pcbnew   # KiCad 9 and 10 (kicad_compat.py)
from make_bus_adapter_pcbs import V, bus_header_pattern, outline, strip

mm = pcbnew.FromMM
xe_pcb, net, adapter_pcb, dst = sys.argv[1:5]

board = pcbnew.LoadBoard(xe_pcb)
keep = tuple(fp.GetReference() for fp in board.GetFootprints() if fp.GetReference() != "J1")
strip(board, keep=keep)
pcbnew.SaveBoard(dst, board)
out = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "netlist_to_pcb.py"),
                      dst, net], check=True, capture_output=True, text=True).stdout
print("netlist_to_pcb:", " / ".join(out.strip().splitlines()))

board = pcbnew.LoadBoard(dst)
j1 = board.FindFootprintByReference("J1")
want = bus_header_pattern(pcbnew.LoadBoard(adapter_pcb).FindFootprintByReference("J2"))
# The XE board's body: x 100..180.64, y 60..100, the right wing out to 196.
# A strip below it for J1 (its courtyard is 9.1 mm across), centred on the
# body, clear of U3's courtyard (to ~y 98.5, on the back too).
X0, XR, YTOP, YBOT = 100.0, 196.0, 60.0, 109.0
CX, CY = 140.32, 104.0
j1.SetPosition(V(CX, CY))
j1.Flip(j1.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
for rot in (0, 90, 180, 270):
    j1.SetOrientationDegrees(rot)
    if bus_header_pattern(j1) == want:
        break
else:
    raise SystemExit(f"no socket orientation matches the header's {want}")
outline(board, [V(X0, YTOP), V(XR, YTOP), V(XR, YBOT), V(X0, YBOT), V(X0, YTOP)])
pcbnew.SaveBoard(dst, board)
pads = {p.GetNumber(): p.GetPosition() for p in j1.Pads()}
print(f"J1 on the back at {CX},{CY}, rotation {rot}: pin 1 at x {pcbnew.ToMM(pads['1'].x):.2f}, "
      f"pin 49 at x {pcbnew.ToMM(pads['49'].x):.2f}; outline {XR - X0:.1f} x {YBOT - YTOP:.1f} mm")
sys.stdout.flush()
os._exit(0)
