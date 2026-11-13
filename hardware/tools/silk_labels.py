#!/usr/bin/python3
"""Plain-English silkscreen labels for a board's headers.

    /usr/bin/python3 hardware/tools/silk_labels.py <board.kicad_pcb>

Adds the labels LABELS lists for the board (by file name): a title per
header, and per-pin labels (from the pin's net, via the spec's net -> text
map). Each label goes at the first candidate spot, around its header or
pin, that's clear of exposed copper (pads, vias), other silkscreen, part
bodies (courtyards) and the board edge (vias are tented: silkscreen may
cross them); on the back silkscreen when the
spec allows and the front has no room. The labels go in a KiCad group named
"silk_labels" (select or move them together in the editor), and a rerun
replaces that group. Also a revision stamp, "REV ${GIT_REV} ${CURRENT_DATE}"
(text variables: export_pcbway.sh fills GIT_REV from git describe; the
project's own value is a placeholder). Silkscreen only: no effect on
placement or routing.

Needs KiCad's pcbnew module (/usr/bin/python3 with the KiCad PPA).
"""
import os
import re
import sys

import pcbnew

mm = pcbnew.FromMM
SIZE, THICK = 0.8, 0.15        # text height, stroke (OSH Park: >= 0.127)
CLEAR = 0.2                    # silkscreen to anything
EDGE_CLEAR = 0.4


def gpio(net):
    """SC_IO19 -> 19, SC_3V3 -> 3V3, GND -> GND, no net -> NC."""
    n = net.lstrip("/")
    if not n or n.startswith("unconnected-"):
        return "NC"
    m = re.fullmatch(r"SC_IO(\d+)", n)
    if m:
        return m.group(1)
    return {"SC_3V3": "3V3", "GND": "GND"}.get(n, n)


def sd(net):
    n = net.lstrip("/")
    m = re.fullmatch(r"SD_DAT(\d)", n)
    if m:
        return "D" + m.group(1)
    return {"SD_CMD": "CMD", "SD_CLK": "CLK", "SC_3V3": "3V3", "GND": "GND"}.get(n, n)


# Per board: (ref, short label beside the header if it fits, legend line,
# per-pin labels beside the pins if they all fit). Every board also gets a
# legend block, "J<n>: name" per header, wherever there's room.
J7_ROWS = "pins"   # legend: the header's pin labels, row by row

BARE_MODULES = [
    ("J3", "IO21", "J3: IO21 1-2 RFSH, 2-3 LINK", {"1": "RFSH", "3": "LINK"}),
    ("J4", "SIDECAR PROG", "J4: SIDECAR PROG", None),
    ("J5", "MAIN PROG", "J5: MAIN PROG, ATARI OFF", None),
    ("J6", "ATARI 5V", "J6: 5V FROM ATARI", None),
    ("J7", "SIDECAR GPIO", ("J7: SIDECAR GPIO", gpio), None),
    ("J8", "NTSC", "J8: NTSC VIDEO (OPTIONAL, R18-R21)", None),
]
LABELS = {
    "sidecar_pcb_xl": BARE_MODULES,
    "sidecar_pcb_xe": BARE_MODULES,
    "espBusBangSidecar": [
        ("J2", "SD CARD", ("J2: SD CARD", sd), None),
        ("J3", "IO21", "J3: IO21 1-2 RFSH, 2-3 LINK", {"1": "RFSH", "3": "LINK"}),
        ("J4", "SIDECAR PROG", "J4: SIDECAR PROG", None),
        ("J5", "MAIN PROG", "J5: MAIN PROG, ATARI OFF", None),
        ("J7", "SIDECAR GPIO", ("J7: SIDECAR GPIO", gpio), None),
        ("J8", "RFSH-IO20", "J8: RFSH ON IO20 (NO USB)", None),
        ("J9", "RST-IO19", "J9: RESET ON IO19 (NO USB)", None),
        ("J10", "SC 5V", "J10: SIDECAR 5V FROM CARRIER", None),
    ],
}
GROUP = "silk_labels"


class Placer:
    def __init__(self, board):
        self.board = board
        self.edge = board.GetBoardEdgesBoundingBox()
        self.edge_poly = pcbnew.SHAPE_POLY_SET()
        board.GetBoardPolygonOutlines(self.edge_poly)
        self.edge_poly.Inflate(-mm(EDGE_CLEAR), pcbnew.CORNER_STRATEGY_CHAMFER_ALL_CORNERS, mm(0.01))
        self.obstacles = {pcbnew.F_SilkS: [], pcbnew.B_SilkS: []}
        # Courtyards as shapes: a box around the WROOM's T-shaped one (the
        # antenna keep-out) would cover half the board.
        self.shapes = {pcbnew.F_SilkS: [], pcbnew.B_SilkS: []}
        front, back = self.obstacles[pcbnew.F_SilkS], self.obstacles[pcbnew.B_SilkS]
        for fp in board.GetFootprints():
            for pad in fp.Pads():
                box = pad.GetBoundingBox()
                if pad.IsOnLayer(pcbnew.F_Cu):
                    front.append(box)
                if pad.IsOnLayer(pcbnew.B_Cu):
                    back.append(box)
            for layer, silk in ((pcbnew.F_CrtYd, pcbnew.F_SilkS), (pcbnew.B_CrtYd, pcbnew.B_SilkS)):
                cy = fp.GetCourtyard(layer)
                if cy.OutlineCount():
                    self.shapes[silk].append(pcbnew.SHAPE_POLY_SET(cy))
            if not fp.GetCourtyard(pcbnew.F_CrtYd).OutlineCount() and not fp.GetCourtyard(pcbnew.B_CrtYd).OutlineCount():
                # No courtyard (the XE's card edge): its whole outline.
                front.append(fp.GetBoundingBox(False))
                back.append(fp.GetBoundingBox(False))
            for item in list(fp.GraphicalItems()) + [f for f in fp.GetFields() if f.IsVisible()]:
                if item.GetLayer() == pcbnew.F_SilkS:
                    front.append(item.GetBoundingBox())
                elif item.GetLayer() == pcbnew.B_SilkS:
                    back.append(item.GetBoundingBox())
        # Vias are tented (setup: tenting front back), so silkscreen may
        # cross them; only exposed copper (pads) is in the way.
        for d in board.GetDrawings():
            if d.GetLayer() in self.obstacles:
                self.obstacles[d.GetLayer()].append(d.GetBoundingBox())

    def fits(self, text, layer):
        box = text.GetBoundingBox()
        for x, y in ((box.GetLeft(), box.GetTop()), (box.GetRight(), box.GetTop()),
                     (box.GetLeft(), box.GetBottom()), (box.GetRight(), box.GetBottom())):
            if not self.edge_poly.Contains(pcbnew.VECTOR2I(x, y)):
                return False
        grown = pcbnew.BOX2I(box.GetOrigin(), box.GetSize())
        grown.Inflate(mm(CLEAR))
        if any(grown.Intersects(o) for o in self.obstacles[layer]):
            return False
        rect = pcbnew.SHAPE_POLY_SET()
        rect.NewOutline()
        for x, y in ((grown.GetLeft(), grown.GetTop()), (grown.GetRight(), grown.GetTop()),
                     (grown.GetRight(), grown.GetBottom()), (grown.GetLeft(), grown.GetBottom())):
            rect.Append(x, y)
        for shape in self.shapes[layer]:
            if shape.BBox().Intersects(grown):
                hit = pcbnew.SHAPE_POLY_SET(rect)
                hit.BooleanIntersection(shape)
                if hit.OutlineCount():
                    return False
        return True

    def undo(self, mark):
        """Remove the labels added since len(self.added) was `mark` (a
        group's items come back unordered, so keep our own list)."""
        while len(self.added) > mark:
            item, layer, box = self.added.pop()
            self.obstacles[layer].remove(box)
            self.group.RemoveItem(item)
            self.board.Delete(item)

    def place(self, content, key, candidates, layers, align=None):
        """candidates: (x_mm, y_mm, angle, side), side one of "down", "up",
        "left", "right": the text goes wholly on that side of the point,
        centred on it the other way. Tried in order, per layer."""
        for layer in layers:
            for x, y, angle, side in candidates:
                t = pcbnew.PCB_TEXT(self.board)
                t.SetText(content)
                t.SetLayer(layer)
                t.SetTextSize(pcbnew.VECTOR2I(mm(SIZE), mm(SIZE)))
                t.SetTextThickness(mm(THICK))
                t.SetHorizJustify(align or pcbnew.GR_TEXT_H_ALIGN_CENTER)
                t.SetVertJustify(pcbnew.GR_TEXT_V_ALIGN_CENTER)
                t.SetTextAngleDegrees(angle)
                t.SetMirrored(layer == pcbnew.B_SilkS)
                t.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
                box = t.GetBoundingBox()
                dx = dy = 0
                if side == "down":
                    dy = mm(y) - box.GetTop()
                elif side == "up":
                    dy = mm(y) - box.GetBottom()
                elif side == "right":
                    dx = mm(x) - box.GetLeft()
                else:
                    dx = mm(x) - box.GetRight()
                t.Move(pcbnew.VECTOR2I(dx, dy))
                if self.fits(t, layer):
                    self.board.Add(t)
                    self.group.AddItem(t)
                    box = t.GetBoundingBox()
                    self.obstacles[layer].append(box)
                    self.added.append((t, layer, box))
                    return layer
        return None


L, R, C = pcbnew.GR_TEXT_H_ALIGN_LEFT, pcbnew.GR_TEXT_H_ALIGN_RIGHT, pcbnew.GR_TEXT_H_ALIGN_CENTER


def title_candidates(box):
    """Around a header's courtyard: below, above, right, left, then further."""
    x0, y0, x1, y1 = (pcbnew.ToMM(v) for v in (box.GetLeft(), box.GetTop(), box.GetRight(), box.GetBottom()))
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    out = []
    for g in (0.1, 0.4, 0.8, 1.5, 2.5, 4.0):
        out += [(cx, y1 + g, 0, "down"), (cx, y0 - g, 0, "up"), (x1 + g, cy, 0, "right"), (x0 - g, cy, 0, "left"),
                (x1 + g, cy, 90, "right"), (x0 - g, cy, 90, "left")]
        for d in (-3, 3, -6, 6, -9, 9):
            out += [(cx + d, y1 + g, 0, "down"), (cx + d, y0 - g, 0, "up"),
                    (x1 + g, cy + d, 0, "right"), (x0 - g, cy + d, 0, "left")]
    return out


def adjacent_candidates(box):
    """Against a header's courtyard, centred on one of its sides."""
    x0, y0, x1, y1 = (pcbnew.ToMM(v) for v in (box.GetLeft(), box.GetTop(), box.GetRight(), box.GetBottom()))
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    out = []
    for g in (0.05, 0.3):
        out += [(cx, y1 + g, 0, "down"), (cx, y0 - g, 0, "up"),
                (x1 + g, cy, 90, "right"), (x0 - g, cy, 90, "left"),
                (x1 + g, cy, 0, "right"), (x0 - g, cy, 0, "left")]
    return out


def pin_candidates(pad_xy, center):
    """Beside a pin, outward from the header's middle first."""
    px, py = pad_xy
    cx, cy = center
    out = []
    horizontal_first = abs(px - cx) >= abs(py - cy)
    for g in (0.95, 1.2, 1.5, 1.8, 2.1, 2.4):
        sides = []
        if px <= cx:
            sides.append((px - g, py, 0, "left"))
        if px >= cx:
            sides.append((px + g, py, 0, "right"))
        vert = [(px, py - g, 90, "up") if py <= cy else (px, py + g, 90, "down")]
        out += sides + vert if horizontal_first else vert + sides
    for g in (0.95, 1.4, 1.9, 2.4):   # a one-row header's other side
        out += [(px + g, py, 0, "right"), (px - g, py, 0, "left")]
    return out


def grid_candidates(board, near):
    """Every spot on a 1 mm grid over the board, nearest to `near` first."""
    box = board.GetBoardEdgesBoundingBox()
    x0, y0, x1, y1 = (pcbnew.ToMM(v) for v in (box.GetLeft(), box.GetTop(), box.GetRight(), box.GetBottom()))
    spots = [(x0 + i, y0 + j) for i in range(int(x1 - x0) + 1) for j in range(int(y1 - y0) + 1)]
    spots.sort(key=lambda s: (s[0] - near[0]) ** 2 + (s[1] - near[1]) ** 2)
    # Level first; then sideways, for tall narrow spaces.
    return [(x, y, 0, "down") for x, y in spots] + [(x, y, 90, "right") for x, y in spots]


def place_anywhere(placer, ref, text, box, layers, center):
    """Next to the header (front, then the back if allowed); else the
    nearest open spot on the board, labelled with the header's reference."""
    for layer in layers:
        if placer.place(text, ref, title_candidates(box), [layer]):
            return True
    far = f"{ref}: {text}" if "\n" not in text else f"{ref}\n{text}"
    for layer in layers:
        if placer.place(far, ref, grid_candidates(placer.board, center), [layer]):
            return True
    return False


def legend_text(fp, texts, title):
    """The pins' labels laid out as the header's rows (as seen from the
    front), under the title: "3V3 GND / 19 20 / ..."."""
    pads = [(texts[n][1].GetPosition(), texts[n][0]) for n in texts]
    rows = {}
    for pos, text in pads:
        rows.setdefault(round(pcbnew.ToMM(pos.y), 1), []).append((pos.x, text))
    if len(rows) < len(pads) / 2:   # a horizontal header: columns are the rows
        rows = {}
        for pos, text in pads:
            rows.setdefault(round(pcbnew.ToMM(pos.x), 1), []).append((-pos.y, text))
    lines = [" ".join(t for _, t in sorted(r)) for _, r in sorted(rows.items())]
    return "\n".join(([title] if title else []) + lines)


def legend_lines(fp, entry):
    """A header's legend line(s): a string, or (heading, net->label) for a
    pin map, one line per header row: "J7: SIDECAR GPIO", "  3V3 GND", ..."""
    if isinstance(entry, str):
        return [entry]
    heading, fn = entry
    pads = sorted(fp.Pads(), key=lambda p: int(p.GetNumber()))
    names = [fn(p.GetNetname()) for p in pads]
    xs = {round(pcbnew.ToMM(p.GetPosition().x), 1) for p in pads}
    ys = {round(pcbnew.ToMM(p.GetPosition().y), 1) for p in pads}
    if len(xs) == 1 or len(ys) == 1:   # one row
        return [f"{heading}, PINS 1-{len(names)}:", "  " + " ".join(names)]
    return [f"{heading} (PIN 1: {names[0]})",
            "  ODD:  " + " ".join(names[0::2]),
            "  EVEN: " + " ".join(names[1::2])]


def main():
    path = sys.argv[1]
    name = os.path.basename(path).replace(".kicad_pcb", "")
    spec = LABELS[name]
    board = pcbnew.LoadBoard(path)
    for g in list(board.Groups()):
        if g.GetName() == GROUP:
            for item in list(g.GetItems()):
                board.Delete(item)
            board.Delete(g)
    placer = Placer(board)
    placer.added = []
    placer.group = pcbnew.PCB_GROUP(board)
    placer.group.SetName(GROUP)
    board.Add(placer.group)
    beside, legend, entries = [], [], []
    for ref, short, line, pins in spec:
        fp = board.FindFootprintByReference(ref)
        box = fp.GetCourtyard(pcbnew.F_CrtYd).BBox()
        center = (pcbnew.ToMM(box.GetCenter().x), pcbnew.ToMM(box.GetCenter().y))
        entries.append(legend_lines(fp, line))
        legend += entries[-1]
        # Beside the header, on the front, right against its outline (further
        # away it could be read as another part's); else the legend has it.
        near = adjacent_candidates(box)
        if short and placer.place(short, ref, near, [pcbnew.F_SilkS]):
            beside.append(short)
        if pins:
            mark = len(placer.added)
            if all(placer.place(text, f"{ref}.{n}", pin_candidates(
                    (pcbnew.ToMM(pad.GetPosition().x), pcbnew.ToMM(pad.GetPosition().y)), center),
                    [pcbnew.F_SilkS])
                   for n, text in pins.items()
                   for pad in [next(p for p in fp.Pads() if p.GetNumber() == n)]):
                beside += list(pins.values())
            else:
                placer.undo(mark)
    # The legend: the nearest open spot to the board's middle, front, then back.
    edge = board.GetBoardEdgesBoundingBox()
    mid = (pcbnew.ToMM(edge.GetCenter().x), pcbnew.ToMM(edge.GetCenter().y))
    where = placer.place("\n".join(legend), "legend", grid_candidates(board, mid),
                         [pcbnew.F_SilkS, pcbnew.B_SilkS], align=L)
    if where:
        report = "legend " + ("on the front" if where == pcbnew.F_SilkS else "on the back")
    else:
        # In pieces, a header's entry each, clustered around the first.
        report, anchor, sides, lost = "legend in pieces", mid, set(), []
        for entry in entries:
            got = placer.place("\n".join(entry), "legend", grid_candidates(board, anchor),
                               [pcbnew.F_SilkS, pcbnew.B_SilkS], align=L)
            if got:
                sides.add("front" if got == pcbnew.F_SilkS else "back")
                p = placer.added[-1][0].GetPosition()
                anchor = (pcbnew.ToMM(p.x), pcbnew.ToMM(p.y))
            else:
                lost.append(entry[0].split(":")[0])
        report += " (" + "/".join(sorted(sides)) + ")" + (f"; NO ROOM for {', '.join(lost)}" if lost else "")
    # The revision stamp: KiCad text variables, filled in when plotting
    # (export_pcbway.sh passes GIT_REV; CURRENT_DATE is built in). Placed
    # with a worst-case stand-in so the real text fits wherever it lands.
    stamp = "REV ${GIT_REV} ${CURRENT_DATE}"
    if placer.place("REV 0123456789-dirty 2026-11-12", "stamp", grid_candidates(board, mid),
                    [pcbnew.F_SilkS, pcbnew.B_SilkS]):
        placer.added[-1][0].SetText(stamp)
        report += "; revision stamp placed"
    else:
        report += "; NO ROOM for the revision stamp"
    pcbnew.SaveBoard(path, board)
    print(f"{name}: beside headers: {', '.join(beside) or 'none'}; {report}")
    sys.stdout.flush()
    os._exit(0)


if __name__ == "__main__":
    main()
