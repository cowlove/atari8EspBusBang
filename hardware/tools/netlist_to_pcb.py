#!/usr/bin/python3
"""Headless "Update PCB from Schematic": bring a board in line with a netlist.

    kicad-cli sch export netlist -o /tmp/x.net board.kicad_sch
    /usr/bin/python3 hardware/tools/netlist_to_pcb.py board.kicad_pcb /tmp/x.net [--stage X Y]

For every netlist component: a footprint missing from the board is loaded
from KiCad's library (/usr/share/kicad/footprints), the project's own
(hardware/<lib>.pretty, e.g. Sidecar.pretty), or the footprint of the
same name already on the board, and dropped in a staging row (--stage, mm;
default below the board) for a placement script to move; a footprint whose
library ID changed is replaced in place (same position, rotation and side).
Every footprint gets its symbol link (path), reference, value, Datasheet
and Description fields, sheet file and "do not populate" flag, and every pad its net (pads the netlist doesn't name: no net).
Footprints with no netlist component are reported, not deleted.

Tracks are left alone: after footprint changes, reroute (autoroute.sh) or
expect DRC to flag what moved.
"""
import argparse
import os
import re

import kicad_netlist
from kicad_compat import pcbnew   # KiCad 9 and 10 (kicad_compat.py)

FP_DIR = "/usr/share/kicad/footprints"
LOCAL_FP_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # hardware/

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("board")
ap.add_argument("netlist")
ap.add_argument("--stage", nargs=2, type=float)
args = ap.parse_args()

text = kicad_netlist.read(args.netlist)   # KiCad 9 or 10 layout
comps = {}
for m in re.finditer(r'\(comp \(ref "([^"]+)"\)\s*\(value "([^"]*)"\)\s*\(footprint "([^"]*)"\)', text):
    end = text.find("(comp ", m.end())
    body = text[m.start():end if end >= 0 else text.find("(nets", m.end())]
    tstamp = re.search(r'\(tstamps "([^"]+)"\)', body).group(1)
    # The Datasheet and Description fields as the schematic has them (KiCad
    # 10's DRC compares them; KiCad 9 writes "~" for no datasheet, 10 an
    # empty field). None: not in the netlist, leave the footprint's.
    fields = {}
    for name in ("Datasheet", "Description"):
        f = re.search(r'\(field \(name "' + name + r'"\)(?: "((?:[^"\\]|\\.)*)")?\)', body)
        fields[name] = (f.group(1) or "") if f else None
    comps[m.group(1)] = dict(value=m.group(2), fpid=m.group(3), uuid=tstamp,
                             dnp='(property (name "dnp"))' in body, fields=fields)
pad_net = {}
for name, body in re.findall(r'\(net \(code "\d+"\) \(name "([^"]+)"\)(.*?)\)\s*(?=\(net |\)\s*\)\s*$)', text, re.S):
    for ref, pin in re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', body):
        pad_net[(ref, pin)] = name

board = pcbnew.LoadBoard(args.board)
sheetfile = os.path.basename(args.board).replace(".kicad_pcb", ".kicad_sch")
on_board = {fp.GetReference(): fp for fp in board.GetFootprints()}
by_fpid = {str(fp.GetFPID().GetUniStringLibId()): fp for fp in board.GetFootprints()}
bb = board.GetBoardEdgesBoundingBox()
sx, sy = (pcbnew.FromMM(v) for v in args.stage) if args.stage else (bb.GetLeft(), bb.GetBottom() + pcbnew.FromMM(10))


def load(fpid):
    lib, name = fpid.split(":", 1)
    for path in (f"{FP_DIR}/{lib}.pretty", f"{LOCAL_FP_DIR}/{lib}.pretty"):
        if os.path.isdir(path):
            fp = pcbnew.FootprintLoad(path, name)
            if fp:
                fp.SetFPID(pcbnew.LIB_ID(lib, name))
                return fp
    if fpid in by_fpid:                  # a library we don't have, but the board does
        return by_fpid[fpid].Duplicate()   # (fresh UUIDs, unlike FOOTPRINT(fp))
    raise SystemExit(f"footprint {fpid} not found")


# Footprint field ids: FIELD_T_* (KiCad 10), else *_FIELD (older bindings).
FIELD_IDS = {
    "Datasheet": getattr(pcbnew, "FIELD_T_DATASHEET", getattr(pcbnew, "DATASHEET_FIELD", None)),
    "Description": getattr(pcbnew, "FIELD_T_DESCRIPTION", getattr(pcbnew, "DESCRIPTION_FIELD", None)),
}

added, replaced = [], []
for ref, c in sorted(comps.items()):
    fp = on_board.get(ref)
    if fp is not None and str(fp.GetFPID().GetUniStringLibId()) != c["fpid"]:
        new = load(c["fpid"])
        new.SetPosition(fp.GetPosition())
        board.Add(new)
        if fp.IsFlipped():
            new.Flip(new.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        new.SetOrientation(fp.GetOrientation())
        board.Delete(fp)
        fp = new
        replaced.append(ref)
    elif fp is None:
        fp = load(c["fpid"])
        fp.SetPosition(pcbnew.VECTOR2I(sx, sy))
        sx += fp.GetBoundingBox(False).GetWidth() + pcbnew.FromMM(2)
        board.Add(fp)
        added.append(ref)
    fp.SetReference(ref)
    fp.SetValue(c["value"])
    fp.SetDNP(c["dnp"])           # the symbol's "do not populate"
    for name, text in c["fields"].items():
        if text is not None and FIELD_IDS.get(name) is not None:
            fp.GetField(FIELD_IDS[name]).SetText(text)
    fp.SetPath(pcbnew.KIID_PATH("/" + c["uuid"]))
    fp.SetSheetname("/")
    fp.SetSheetfile(sheetfile)
    for pad in fp.Pads():
        name = pad_net.get((ref, pad.GetNumber()))
        if not name:
            pad.SetNetCode(0)
            continue
        net = board.FindNet(name)
        if net is None:
            net = pcbnew.NETINFO_ITEM(board, name)
            board.Add(net)
        pad.SetNet(net)

extra = sorted(set(fp.GetReference() for fp in board.GetFootprints()) - set(comps))
pcbnew.SaveBoard(args.board, board)
print(f"added {len(added)}: {' '.join(added)}")
print(f"replaced {len(replaced)}: {' '.join(replaced)}")
if extra:
    print(f"on the board, not in the netlist: {' '.join(extra)}")
import sys; sys.stdout.flush()
os._exit(0)   # skip pcbnew's teardown (it can crash at exit)
