#!/usr/bin/python3
"""Build the two bus adapter boards (unrouted) from their schematics' netlists.

    bus_adapter_pbi: J1 the XL board's PBI socket footprint (5530843-5;
        footprint on F.Cu, connector soldered from the back, as on the XL
        board), J2 the 2x25 header on the front, its pins in line with J1's
        (header pin n above socket pin n, so each net is a short vertical run).
    bus_adapter_eci: J1 the eci2Pbi adapter's cartridge + ECI fingers, turned
        as on sidecar_pcb_xe (tabs down), J2 the header on the front of a body
        as wide as the tabs.

Both headers have the same rotation and side, so one socket orientation on
sidecar_pcb_hdr mates both (make_sidecar_hdr_pcb.py checks its pads
against bus_header_pattern() here).

    for b in pbi eci; do kicad-cli sch export netlist -o /tmp/$b.net hardware/bus_adapter_$b/bus_adapter_$b.kicad_sch; done
    /usr/bin/python3 hardware/tools/make_bus_adapter_pcbs.py hardware/sidecar_pcb_xl/sidecar_pcb_xl.kicad_pcb \\
        hardware/eci2Pbi/atari8EciToPbi.kicad_pcb /tmp/pbi.net /tmp/eci.net hardware

Then autoroute.sh each board. The base board (layers, rules) is the XL
board with everything removed.
"""
import os
import subprocess
import sys

from kicad_compat import pcbnew   # KiCad 9 and 10 (kicad_compat.py)

mm = pcbnew.FromMM
TOOLS = os.path.dirname(os.path.abspath(__file__))
HEADER_ROT = 90      # pins along +x, pin 2's row above pin 1's (-y)


def V(x, y):
    return pcbnew.VECTOR2I(mm(x), mm(y))


def bus_header_pattern(fp):
    """Unit directions pin 1 -> pin 2 (across the rows) and pin 1 -> pin 3 (along them), board coordinates (top view)."""
    pads = {p.GetNumber(): p.GetPosition() for p in fp.Pads()}

    def unit(a, b):
        dx, dy = pads[b].x - pads[a].x, pads[b].y - pads[a].y
        return (round(dx / max(abs(dx), abs(dy))), round(dy / max(abs(dx), abs(dy))))
    return unit("1", "2"), unit("1", "3")


def strip(board, keep=()):
    for t in list(board.GetTracks()):
        board.Delete(t)
    for z in list(board.Zones()):
        board.Delete(z)
    for d in list(board.GetDrawings()):
        board.Delete(d)
    for fp in list(board.GetFootprints()):
        if fp.GetReference() not in keep:
            board.Delete(fp)


def outline(board, pts):
    for p, q in zip(pts, pts[1:]):
        seg = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
        seg.SetLayer(pcbnew.Edge_Cuts)
        seg.SetWidth(mm(0.1))
        seg.SetStart(p)
        seg.SetEnd(q)
        board.Add(seg)


def note(board, text, x, y):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(text)
    t.SetLayer(pcbnew.F_SilkS)
    t.SetTextSize(V(1.0, 1.0))
    t.SetTextThickness(mm(0.15))
    t.SetPosition(V(x, y))
    board.Add(t)


def link(dst, net):
    out = subprocess.run([sys.executable, os.path.join(TOOLS, "netlist_to_pcb.py"), dst, net],
                         check=True, capture_output=True, text=True).stdout
    print("  netlist_to_pcb:", " / ".join(out.strip().splitlines()))


def build_pbi(xl_pcb, net, dst):
    board = pcbnew.LoadBoard(xl_pcb)
    strip(board, keep=("J1",))
    pcbnew.SaveBoard(dst, board)
    link(dst, net)                      # J1's symbol link and nets; J2 from the library
    board = pcbnew.LoadBoard(dst)
    j1, j2 = board.FindFootprintByReference("J1"), board.FindFootprintByReference("J2")
    # J1: pins along +x from (X, Y), even row 4.85 mm below. J2: its odd
    # row 5.08 mm above J1's (clear of J1's courtyard), even row above that.
    X, Y = 100.0, 100.0
    j1.SetOrientationDegrees(0)
    j1.SetPosition(V(X, Y))
    j2.SetOrientationDegrees(HEADER_ROT)
    j2.SetPosition(V(X, Y - 5.08))
    p1 = {p.GetNumber(): p.GetPosition() for p in j2.Pads()}["1"]
    j2.Move(V(X, Y - 5.08) - p1)        # pad 1 exactly over socket pad 1's column
    x0, x1, y0, y1 = X - 7.5, X + 61.0 + 7.5, Y - 5.08 - 2.54 - 3.0, Y + 4.85 + 3.5
    outline(board, [V(x0, y0), V(x1, y0), V(x1, y1), V(x0, y1), V(x0, y0)])
    note(board, "J1: mount on BACK side", X + 30.5, y1 - 1.0)
    pcbnew.SaveBoard(dst, board)
    return board


def build_eci(xl_pcb, eci_pcb, net, dst):
    board = pcbnew.LoadBoard(xl_pcb)
    strip(board)
    src = pcbnew.LoadBoard(eci_pcb)     # keep: the footprint belongs to it
    j1 = pcbnew.FOOTPRINT(src.FindFootprintByReference("J1"))
    board.Add(j1)
    pcbnew.SaveBoard(dst, board)
    link(dst, net)
    board = pcbnew.LoadBoard(dst)
    j1, j2 = board.FindFootprintByReference("J1"), board.FindFootprintByReference("J2")
    # J1 as on sidecar_pcb_xe: turned 180, tabs below y 100, x 100..180.64.
    X0, X1, YTAB, YTOP = 100.0, 180.64, 100.0, 78.0
    j1.SetOrientationDegrees(180)
    j1.SetPosition(V(X1 + 0.016, YTAB - 0.055))
    j2.SetOrientationDegrees(HEADER_ROT)
    pin1 = V((X0 + X1) / 2 - 30.48, YTOP + 7.0)
    j2.SetPosition(pin1)
    j2.Move(pin1 - {p.GetNumber(): p.GetPosition() for p in j2.Pads()}["1"])
    # Close the tabs' open outline around the body.
    ends = {}
    for g in j1.GraphicalItems():
        if g.GetLayer() == pcbnew.Edge_Cuts:
            for p in (g.GetStart(), g.GetEnd()):
                ends[(p.x, p.y)] = ends.get((p.x, p.y), 0) + 1
    (ax, ay), (bx, by) = sorted(k for k, n in ends.items() if n == 1)
    top = mm(YTOP)
    outline(board, [pcbnew.VECTOR2I(ax, ay), pcbnew.VECTOR2I(ax, top), pcbnew.VECTOR2I(bx, top),
                    pcbnew.VECTOR2I(bx, by)])
    pcbnew.SaveBoard(dst, board)
    return board


if __name__ == "__main__":
    xl_pcb, eci_pcb, pbi_net, eci_net, outdir = sys.argv[1:6]
    pats = []
    for name, build in (("bus_adapter_pbi", lambda d: build_pbi(xl_pcb, pbi_net, d)),
                        ("bus_adapter_eci", lambda d: build_eci(xl_pcb, eci_pcb, eci_net, d))):
        dst = os.path.join(outdir, name, name + ".kicad_pcb")
        print(name)
        board = build(dst)
        bb = board.GetBoardEdgesBoundingBox()
        j2 = board.FindFootprintByReference("J2")
        pats.append(bus_header_pattern(j2))
        nets = sum(1 for fp in board.GetFootprints() for p in fp.Pads() if p.GetNetCode())
        print(f"  outline {pcbnew.ToMM(bb.GetWidth()):.2f} x {pcbnew.ToMM(bb.GetHeight()):.2f} mm; "
              f"{nets} pads on nets; header pattern (1->2, 1->3) {pats[-1]}")
    assert pats[0] == pats[1], pats
    sys.stdout.flush()
    os._exit(0)
