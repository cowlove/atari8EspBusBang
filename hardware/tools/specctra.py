#!/usr/bin/python3
"""Specctra DSN export and SES import through KiCad's pcbnew module.

    specctra.py export <board.kicad_pcb> <out.dsn> [--skip-net NET]... [--inset-mm MM]
    specctra.py import <board.kicad_pcb> <routed.ses> <out.kicad_pcb> [--pour NET]
    specctra.py cleanup <board.kicad_pcb> <drc.json> --net NET

export --skip-net drops a net from the DSN's netlist (its pads stay, as
obstacles): for GND, which pours connect instead of traces. --inset-mm
shrinks the (rectangular) routing boundary, since Freerouting ignores
KiCad's copper-to-edge clearance.

import --pour adds a zone for NET on both copper layers over the whole board
and fills it, after the routes are in.

cleanup deletes NET's tracks that a kicad-cli DRC (--format json) names in
clearance errors, and refills the zones, in place: Freerouting now and then
leaves a GND track too close to something, and the pour carries GND anyway.
Rerun DRC afterwards to check the pour still connects everything.

Needs the system Python that KiCad's packages install pcbnew for (on miner6,
/usr/bin/python3 with the KiCad 9 PPA). Used by autoroute.sh.
"""
import argparse
import re

import pcbnew


def export(args):
    board = pcbnew.LoadBoard(args.board)
    if not pcbnew.ExportSpecctraDSN(board, args.dsn):
        raise SystemExit("DSN export failed")
    dsn = open(args.dsn).read()
    for net in args.skip_net:
        name = re.escape("/" + net.lstrip("/"))
        # The net's pin list in (network ...), and its name in any (class ...).
        dsn, n = re.subn(r'\n\s*\(net ' + name + r'\s*\(pins[^)]*\)\s*\)', '', dsn)
        dsn = re.sub(r'(\(class [^\n(]*?)\s' + name + r'(?=[\s)])', r'\1', dsn)
        print(f"skipped net {net}: {n} pin list removed")
    if args.inset_mm:
        # The board outline shrunk by the inset, as DSN paths (um, y up).
        polys = pcbnew.SHAPE_POLY_SET()
        board.GetBoardPolygonOutlines(polys)
        polys.Inflate(-pcbnew.FromMM(args.inset_mm), pcbnew.CORNER_STRATEGY_CHAMFER_ALL_CORNERS,
                      pcbnew.FromMM(0.01))
        assert polys.OutlineCount() == 1, "outline splits when inset"
        chain = polys.Outline(0)
        pts = [chain.CPoint(i) for i in range(chain.PointCount())]
        pts.append(pts[0])
        path = "  ".join(f"{round(p.x / 1000)} {round(-p.y / 1000)}" for p in pts)
        m = re.search(r'\(boundary\s*\(path pcb 0([^)]*)\)', dsn)
        dsn = dsn[:m.start(1)] + "  " + path + dsn[m.end(1):]
        print(f"routing boundary inset {args.inset_mm} mm ({len(pts) - 1} corners)")
        # Freerouting cuts concave corners of the boundary (the notch between
        # card-edge tabs): keep it out of whatever of the bounding box is
        # off the board, explicitly.
        box = polys.BBox()
        off = pcbnew.SHAPE_POLY_SET()
        off.NewOutline()
        for x, y in ((box.GetLeft(), box.GetTop()), (box.GetRight(), box.GetTop()),
                     (box.GetRight(), box.GetBottom()), (box.GetLeft(), box.GetBottom())):
            off.Append(x, y)
        off.BooleanSubtract(polys)
        # It also strays a little past straight edges: fence the box too.
        L, T, R, B = box.GetLeft(), box.GetTop(), box.GetRight(), box.GetBottom()
        w = pcbnew.FromMM(5)
        for x0, y0, x1, y1 in ((L - w, T - w, L, B + w), (R, T - w, R + w, B + w),
                               (L, T - w, R, T), (L, B, R, B + w)):
            off.NewOutline()
            for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
                off.Append(x, y)
        keepouts = ""
        for i in range(off.OutlineCount()):
            c = off.Outline(i)
            q = [c.CPoint(j) for j in range(c.PointCount())] + [c.CPoint(0)]
            poly = "  ".join(f"{round(p.x / 1000)} {round(-p.y / 1000)}" for p in q)
            for layer in ("F.Cu", "B.Cu"):
                keepouts += f'\n    (keepout "" (polygon {layer} 0  {poly}))'
        end = dsn.index(")", dsn.index(")", m.start(1) + len(path) + 2) + 1) + 1
        dsn = dsn[:end] + keepouts + dsn[end:]
        print(f"  {off.OutlineCount()} off-board keepouts (4 of them fencing the box)")
    open(args.dsn, "w").write(dsn)


def add_pour(board, netname):
    net = board.FindNet(netname) or board.FindNet("/" + netname.lstrip("/"))
    if net is None:
        raise SystemExit(f"no net {netname}")
    box = board.GetBoardEdgesBoundingBox()
    corners = [(box.GetLeft(), box.GetTop()), (box.GetRight(), box.GetTop()),
               (box.GetRight(), box.GetBottom()), (box.GetLeft(), box.GetBottom())]
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        zone = pcbnew.ZONE(board)
        zone.SetLayer(layer)
        zone.SetNet(net)
        zone.SetIsRuleArea(False)
        zone.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        zone.SetMinThickness(pcbnew.FromMM(0.25))
        outline = zone.Outline()
        outline.NewOutline()
        for x, y in corners:
            outline.Append(x, y)
        board.Add(zone)
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    # Stitch the two layers' pours together wherever a via fits inside both
    # fills: the tracks cut each pour into islands, and the vias join them.
    # Each via goes where it joins two islands (one per layer) not yet
    # connected (union-find), or one island to a GND pad's island; a fine
    # grid finds the narrow islands. Holes keep 0.3 mm apart.
    fills = [z.GetFilledPolysList(z.GetLayer()) for z in board.Zones() if z.GetNetCode() == net.GetNetCode()]
    dia, drill, step = pcbnew.FromMM(0.6), pcbnew.FromMM(0.3), pcbnew.FromMM(0.5)
    r = dia // 2
    ring = [(0, 0)] + [(round(r * c), round(r * s)) for c, s in
                       ((1, 0), (-1, 0), (0, 1), (0, -1), (.7, .7), (-.7, .7), (.7, -.7), (-.7, -.7))]
    holes = [(p.GetPosition(), max(p.GetDrillSize().x, p.GetDrillSize().y) // 2)
             for p in board.GetPads() if p.GetDrillSize().x > 0]
    holes += [(v.GetPosition(), v.GetDrill() // 2) for v in board.GetTracks() if v.GetClass() == "PCB_VIA"]
    gap = pcbnew.FromMM(0.3)
    parent = {}

    def find(k):
        while parent.setdefault(k, k) != k:
            parent[k] = parent[parent[k]]
            k = parent[k]
        return k

    def island(li, pt):
        f = fills[li]
        for i in range(f.OutlineCount()):
            if f.Outline(i).PointInside(pt) and not any(
                    f.Hole(i, h).PointInside(pt) for h in range(f.HoleCount(i))):
                return (li, i)
        return None

    # Through-hole GND pads already join the islands they sit in.
    for p in board.GetPads():
        if p.GetNetCode() == net.GetNetCode() and p.GetDrillSize().x > 0:
            ids = [island(li, p.GetPosition()) for li in range(len(fills))]
            for a in ids[1:]:
                if a and ids[0]:
                    parent[find(a)] = find(ids[0])
    n = 0
    for x in range(box.GetLeft() + step // 2, box.GetRight(), step):
        for y in range(box.GetTop() + step // 2, box.GetBottom(), step):
            pt = pcbnew.VECTOR2I(x, y)
            if not all(f.Contains(pcbnew.VECTOR2I(x + dx, y + dy)) for f in fills for dx, dy in ring):
                continue
            ids = [island(li, pt) for li in range(len(fills))]
            if None in ids or len({find(i) for i in ids}) == 1:
                continue
            if any((pt - c).EuclideanNorm() < hr + drill // 2 + gap for c, hr in holes):
                continue
            via = pcbnew.PCB_VIA(board)
            via.SetPosition(pt)
            via.SetWidth(dia)
            via.SetDrill(drill)
            via.SetNet(net)
            board.Add(via)
            holes.append((pt, drill // 2))
            for a in ids[1:]:
                parent[find(a)] = find(ids[0])
            n += 1
    filler.Fill(board.Zones())
    print(f"poured {net.GetNetname()} on both layers, {n} stitching vias")


def import_(args):
    board = pcbnew.LoadBoard(args.board)
    if not pcbnew.ImportSpecctraSES(board, args.ses):
        raise SystemExit("SES import failed")
    for net in args.pour:
        add_pour(board, net)
    pcbnew.SaveBoard(args.out, board)
    tracks = list(board.GetTracks())
    vias = sum(1 for t in tracks if t.GetClass() == "PCB_VIA")
    print(f"imported: {len(tracks) - vias} track segments, {vias} vias")


def cleanup(args):
    import json
    board = pcbnew.LoadBoard(args.board)
    net = "/" + args.net.lstrip("/")
    bad = {it["uuid"] for v in json.load(open(args.drc))["violations"] if v["type"] == "clearance"
           for it in v["items"]}
    n = 0
    for t in list(board.GetTracks()):
        if t.GetClass() == "PCB_TRACK" and t.GetNetname() == net and t.m_Uuid.AsString() in bad:
            board.Delete(t)
            n += 1
    if n:
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        pcbnew.SaveBoard(args.board, board)
    print(f"cleanup: deleted {n} {net} tracks with clearance errors")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export")
    e.add_argument("board")
    e.add_argument("dsn")
    e.add_argument("--skip-net", action="append", default=[])
    e.add_argument("--inset-mm", type=float, default=0.0)
    i = sub.add_parser("import")
    i.add_argument("board")
    i.add_argument("ses")
    i.add_argument("out")
    i.add_argument("--pour", action="append", default=[])
    c = sub.add_parser("cleanup")
    c.add_argument("board")
    c.add_argument("drc")
    c.add_argument("--net", required=True)
    args = ap.parse_args()
    {"export": export, "import": import_, "cleanup": cleanup}[args.cmd](args)


if __name__ == "__main__":
    main()
