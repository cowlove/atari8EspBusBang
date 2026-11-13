#!/usr/bin/env python3
"""Generate the sidecar_pcb_xe schematic: sidecar_pcb_xl on the XE's cartridge + ECI slots.

Starts from the sidecar_pcb_xl schematic and its netlist and swaps the PBI
edge connector J1 for the XE's cartridge + ECI card edge (the Atari8:CART+ECI
symbol, copied from the eci2Pbi adapter's schematic). Each card-edge pin gets
the XL board's net for the PBI pin the eci2Pbi adapter wires it to, so the
circuit is otherwise identical. PBI pins with no ECI counterpart (EXTDEC,
CAS, RAS, the NCs) are left out: nothing else on the board uses them. The
cartridge-only pins (S4, S5, RD4, RD5, CCTL) and ECI D1XX and RESERVED stay
unconnected, as on the adapter.

    kicad-cli sch export netlist -o /tmp/xl.net hardware/sidecar_pcb_xl/sidecar_pcb_xl.kicad_sch
    kicad-cli sch export netlist -o /tmp/eci.net hardware/eci2Pbi/atari8EciToPbi.kicad_sch
    python3 hardware/tools/make_sidecar_xe_schematic.py \\
        hardware/sidecar_pcb_xl/sidecar_pcb_xl.kicad_sch /tmp/xl.net \\
        hardware/eci2Pbi/atari8EciToPbi.kicad_sch /tmp/eci.net \\
        hardware/sidecar_pcb_xe/sidecar_pcb_xe.kicad_sch

Deterministic UUIDs: reruns only show changes. Prints the pin map.
"""
import re
import sys
import uuid

import kicad_netlist

xl_sch, xl_net, eci_sch, eci_net, dst = sys.argv[1:6]
s = open(xl_sch).read()

_n = 0


def U():
    global _n
    _n += 1
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"atari8EspBusBang/sidecar_pcb_xe/{_n}"))


def block(text, start):
    d = 0
    for j in range(start, len(text)):
        if text[j] == '(':
            d += 1
        elif text[j] == ')':
            d -= 1
            if d == 0:
                return text[start:j + 1]
    raise ValueError("unbalanced")


NET_RE = r'\(net \(code "\d+"\) \(name "([^"]+)"\)(.*?)\)\s*(?=\(net |\)\s*\)\s*$)'
PIN_RE = (r'\(pin \w+ \w+\s*\(at ([-\d.]+) ([-\d.]+) (\d+)\)\s*\(length [\d.]+\)'
          r'(?:\s*\(hide yes\))?.*?\(name "([^"]*)".*?\(number "([^"]*)"')


def pin_nets(net_file, ref):
    out = {}
    for name, body in re.findall(NET_RE, kicad_netlist.read(net_file), re.S):
        for pin in re.findall(r'\(node \(ref "' + ref + r'"\) \(pin "([^"]+)"\)', body):
            out[pin] = name
    return out


# --- ECI pin -> PBI pin, from the adapter (its J1 = CART+ECI, J2 = PBI).
eci_by_pin, pbi_by_pin = pin_nets(eci_net, "J1"), pin_nets(eci_net, "J2")
pbi_pins_of = {}
for pin, net in pbi_by_pin.items():
    pbi_pins_of.setdefault(net, []).append(pin)
xl_pbi = pin_nets(xl_net, "J1")            # XL board: PBI pin -> net
xe_nets = {}
for pin, net in eci_by_pin.items():
    if net.startswith("unconnected-") or net not in pbi_pins_of:
        continue
    # VCC50 is two PBI pins (47, 48) on the XL board as two nets; the XL
    # board's jumper J6 takes 47 (VCC50_1), so both ECI 5 V pins join it.
    pbi = sorted(pbi_pins_of[net], key=int)[0]
    xe_nets[pin] = xl_pbi[pbi]

# --- Drop the PBI connector J1 and the labels on its pins.
CONN = "CONN50_5530843-5_TEC:5530843-5"
m = re.search(r'\n\t\(symbol\n\t\t\(lib_id "' + re.escape(CONN) + r'"\)\n\t\t\(at ([-\d.]+) ([-\d.]+) 0\)', s)
jx, jy = float(m.group(1)), float(m.group(2))
start = m.start() + 1
s = s[:start] + s[start + len(block(s, start)):]
lib = block(s, s.index(f'(symbol "{CONN}"'))
spots = {(round(jx + float(x), 2), round(jy - float(y), 2)) for x, y, *_ in re.findall(PIN_RE, lib, re.S)}


dropped = []


def drop_label(m):
    x, y = round(float(m.group(2)), 2), round(float(m.group(3)), 2)
    if (x, y) in spots:
        dropped.append(m.group(1))
        return ""
    return m.group(0)


s = re.sub(r'\t\(label "([^"]*)"\n\t\t\(at ([-\d.]+) ([-\d.]+) \d+\)\n(?:\t\t[^\n]*\n)*?\t\)\n',
           drop_label, s)
# A wire from a part to a J1 pin (R1 to RESET): drop it, and label its other
# end with that pin's net instead.
moved = []
for net in re.finditer(r'\t\(wire\n\t\t\(pts\n\t\t\t\(xy ([-\d.]+) ([-\d.]+)\) \(xy ([-\d.]+) ([-\d.]+)\)\n'
                       r'(?:\t\t[^\n]*\n)*?\t\)\n', s):
    a = (round(float(net.group(1)), 2), round(float(net.group(2)), 2))
    b = (round(float(net.group(3)), 2), round(float(net.group(4)), 2))
    for pin_end, far in ((a, b), (b, a)):
        if pin_end in spots:
            moved.append((net.group(0), far, pin_end))
label_at = {}
for name, x, y in re.findall(r'\(label "([^"]*)"\n\t\t\(at ([-\d.]+) ([-\d.]+) \d+\)', open(xl_sch).read()):
    label_at[(round(float(x), 2), round(float(y), 2))] = name
wire_labels = []
for text, far, pin_end in moved:
    s = s.replace(text, "", 1)
    wire_labels.append((label_at[pin_end], far))

k = s.index(f'(symbol "{CONN}"')
k = s.rindex('\n', 0, k) + 1
s = s[:k] + s[k + len(lib) + s[k:].index('(') + 1:]

# --- The CART+ECI symbol, from the adapter's embedded library.
e = open(eci_sch).read()
cart_lib = block(e, e.index('(symbol "Atari8:CART+ECI"'))
cart_pins = {num: (float(x), float(y), int(a)) for x, y, a, _, num in re.findall(PIN_RE, cart_lib, re.S)}
end = s.index('\t(lib_symbols')
end = end + 1 + len(block(s, end + 1)) - 1
s = s[:end].rstrip('\t') + "\t\t" + cart_lib + "\n\t" + s[end:]

# Pins (all on the symbol's left, at x -10.16) land where J1's were: x on
# J1's pin column, the top pin on J1's first.
cx = jx + 10.16
cy = jy + max(y for _, y, _ in cart_pins.values())
ROOT = re.search(r'\(uuid "([^"]+)"\)', s).group(1)
FP = "Atari8:CART+ECI_V3.0"


def prop(name, value, x, y, hide=False):
    h = "\n\t\t\t\t(hide yes)" if hide else ""
    return (f'\t\t(property "{name}" "{value}"\n\t\t\t(at {x:g} {y:g} 0)\n\t\t\t(effects\n'
            f'\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t){h}\n\t\t\t)\n\t\t)\n')


sym_uuid = U()
pins_txt = "".join(f'\t\t(pin "{p}"\n\t\t\t(uuid "{U()}")\n\t\t)\n' for p in cart_pins)
out = [f'\t(symbol\n\t\t(lib_id "Atari8:CART+ECI")\n\t\t(at {cx:g} {cy:g} 0)\n\t\t(unit 1)\n'
       f'\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)\n'
       f'\t\t(uuid "{sym_uuid}")\n'
       + prop("Reference", "J1", cx + 2.54, cy - 56.0)
       + prop("Value", "CART+ECI", cx + 2.54, cy - 53.5)
       + prop("Footprint", FP, cx, cy, True) + prop("Datasheet", "~", cx, cy, True)
       + prop("Description", "XE cartridge + ECI card edge", cx, cy, True) + pins_txt
       + f'\t\t(instances\n\t\t\t(project ""\n\t\t\t\t(path "/{ROOT}"\n'
         f'\t\t\t\t\t(reference "J1")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n']
LABEL = ('\t(label "{name}"\n\t\t(at {x:g} {y:g} 180)\n\t\t(effects\n\t\t\t(font\n'
         '\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify right bottom)\n\t\t)\n\t\t(uuid "{u}")\n\t)\n')
for pin in sorted(xe_nets, key=lambda p: -cart_pins[p][1]):
    px, py, _ = cart_pins[pin]
    out.append(LABEL.format(name=xe_nets[pin].lstrip('/'), x=round(cx + px, 4), y=round(cy - py, 4), u=U()))
for name, (x, y) in wire_labels:
    out.append(LABEL.replace(" 180)", " 0)").replace("right bottom", "left bottom")
               .format(name=name, x=x, y=y, u=U()))
# No-connect flags on the pins the adapter leaves open.
for pin in sorted(set(cart_pins) - set(xe_nets)):
    px, py, _ = cart_pins[pin]
    out.append(f'\t(no_connect\n\t\t(at {round(cx + px, 4):g} {round(cy - py, 4):g})\n\t\t(uuid "{U()}")\n\t)\n')

# Text notes: PBI wording -> ECI.
s = s.replace("J6: PBI 5V (VCC50_1)", "J6: cartridge/ECI 5V (VCC50_1)")
NOTE = ("sidecar_pcb_xe: sidecar_pcb_xl on the XE's cartridge + ECI slots. J1 is the card edge "
        "(cartridge + ECI, as on the eci2Pbi adapter); each pin carries the XL board's net for the PBI pin "
        "the adapter maps it to. ECI C1 (HALT) = PBI 36 (READY), as on the adapter. No ECI pin for "
        "EXTDEC, CAS or RAS, which this board doesn't use.")
out.append(f'\t(text "{NOTE}"\n\t\t(exclude_from_sim no)\n\t\t(at {jx - 20:g} {cy + 85:g} 0)\n\t\t(effects\n'
           f'\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify left bottom)\n\t\t)\n'
           f'\t\t(uuid "{U()}")\n\t)\n')
k = s.index('\t(sheet_instances')
s = s[:k] + "".join(out) + s[k:]
open(dst, "w").write(s)
print(f"dropped J1 ({len(spots)} pins, {len(dropped)} labels); CART+ECI at {cx:g},{cy:g}")
for pin in sorted(xe_nets, key=lambda p: -cart_pins[p][1]):
    print(f"  ECI {pin:>4} -> {xe_nets[pin]}")
missing = sorted(set(xl_pbi.values()) - set(xe_nets.values()))
print("XL PBI nets without an ECI pin:", ", ".join(missing))
