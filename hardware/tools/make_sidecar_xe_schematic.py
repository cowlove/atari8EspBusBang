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

XE-only additions (after everything else, so the XL parts keep their UUIDs):
the Sidecar's USB on a USB-C receptacle J9 (D-/D+ on IO19/IO20, 5.1k CC
pull-downs R23/R24, VBUS through Schottky D3 to the board rail), and the
Sidecar's own drive of the Atari's RESET: IO3 (SC_RESET_DRV) through R22,
0 ohm, as main's IO19 does through R1. IO3 leaves the breakout J7 (pin 5 NC).

    kicad-cli sch export netlist -o /tmp/xl.net hardware/sidecar_pcb_xl/sidecar_pcb_xl.kicad_sch
    kicad-cli sch export netlist -o /tmp/eci.net hardware/eci2Pbi/atari8EciToPbi.kicad_sch
    python3 hardware/tools/make_sidecar_xe_schematic.py \\
        hardware/sidecar_pcb_xl/sidecar_pcb_xl.kicad_sch /tmp/xl.net \\
        hardware/eci2Pbi/atari8EciToPbi.kicad_sch /tmp/eci.net \\
        hardware/sidecar_pcb_xe/sidecar_pcb_xe.kicad_sch hardware/symbols

The symbols directory is hardware/symbols (vendored KiCad 9.0.7 symbols, as
for make_sidecar_xl_schematic.py): the USB-C receptacle and the Schottky.

Deterministic UUIDs: reruns only show changes. Prints the pin map.
"""
import re
import sys
import uuid

import kicad_netlist

xl_sch, xl_net, eci_sch, eci_net, dst, symdir = sys.argv[1:7]
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
# --- XE-only additions: Sidecar USB-C (J9) and the Sidecar's RESET drive
# (R22). New UUIDs come after all of the above, so nothing earlier moves.


def lib_symbol(libfile, name):
    t = open(f"{symdir}/{libfile}.kicad_sym").read()
    b = block(t, t.index(f'(symbol "{name}"'))
    b = b.replace(f'(symbol "{name}"', f'(symbol "{libfile}:{name}"', 1)
    return "\t\t" + b.replace("\n", "\n\t") + "\n"


def pins_of(lib_text):
    return {num: (float(x), float(y), int(a)) for x, y, a, _, num in re.findall(PIN_RE, lib_text, re.S)}


# The USB-C symbol's shield pin is S1; the GCT USB4105 footprint's shield
# pads are SH. Renumber the embedded symbol's pin to match the footprint.
usb_lib = lib_symbol("Connector", "USB_C_Receptacle_USB2.0_16P").replace('(number "S1"', '(number "SH"')
schottky_lib = lib_symbol("Device", "D_Schottky")
end = s.index('\t(lib_symbols')
end = end + 1 + len(block(s, end + 1)) - 1
s = s[:end].rstrip('\t') + usb_lib + schottky_lib + "\t" + s[end:]
res_lib = block(s, s.index('(symbol "Device:R"'))
usb_pins, schottky_pins, res_pins = pins_of(usb_lib), pins_of(schottky_lib), pins_of(res_lib)

# IO3: U2's SC_IO3 label becomes SC_RESET_DRV; J7's (pin 5) goes, with a
# no-connect flag in its place.
U2_IO3 = (234.95, 101.6)
J7_PIN5 = (381.0, 60.96)
n_io3 = 0
for m in list(re.finditer(r'\(label "SC_IO3"\n\t\t\(at ([-\d.]+) ([-\d.]+) \d+\)', s)):
    n_io3 += 1
    assert (float(m.group(1)), float(m.group(2))) in (U2_IO3, J7_PIN5), m.groups()
assert n_io3 == 2, n_io3
s = re.sub(r'\(label "SC_IO3"(\n\t\t\(at 234\.95 101\.6 \d+\))', r'(label "SC_RESET_DRV"\1', s)
s = re.sub(r'\t\(label "SC_IO3"\n\t\t\(at 381 60\.96 \d+\)\n(?:\t\t[^\n]*\n)*?\t\)\n', "", s)
add = [f'\t(no_connect\n\t\t(at {J7_PIN5[0]:g} {J7_PIN5[1]:g})\n\t\t(uuid "{U()}")\n\t)\n']


def place(lib_id, ref, value, footprint, x, y, pinmap, nets, description=""):
    """A symbol (no rotation), a label on each pin in nets, a no-connect on
    each pin nets maps to None. Stacked pins get one label."""
    pins_txt = "".join(f'\t\t(pin "{p}"\n\t\t\t(uuid "{U()}")\n\t\t)\n' for p in pinmap)
    add.append(f'\t(symbol\n\t\t(lib_id "{lib_id}")\n\t\t(at {x:g} {y:g} 0)\n\t\t(unit 1)\n'
               f'\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)\n'
               f'\t\t(uuid "{U()}")\n'
               + prop("Reference", ref, x + 2.54, y - 1.27) + prop("Value", value, x + 2.54, y + 1.27)
               + prop("Footprint", footprint, x, y, True) + prop("Datasheet", "~", x, y, True)
               + prop("Description", description, x, y, True) + pins_txt
               + f'\t\t(instances\n\t\t\t(project ""\n\t\t\t\t(path "/{ROOT}"\n'
                 f'\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n')
    done = set()
    for num, net in nets.items():
        px, py, ang = pinmap[num]
        spot = (round(x + px, 4), round(y - py, 4))
        if spot in done:
            continue
        done.add(spot)
        if net is None:
            add.append(f'\t(no_connect\n\t\t(at {spot[0]:g} {spot[1]:g})\n\t\t(uuid "{U()}")\n\t)\n')
            continue
        a = {180: 0, 0: 180, 270: 90, 90: 270}[ang]
        add.append(f'\t(label "{net}"\n\t\t(at {spot[0]:g} {spot[1]:g} {a})\n\t\t(effects\n\t\t\t(font\n'
                   f'\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify {"left" if a in (0, 90) else "right"} bottom)\n'
                   f'\t\t)\n\t\t(uuid "{U()}")\n\t)\n')


R_FP = "Resistor_SMD:R_0805_2012Metric_Pad1.20x1.40mm_HandSolder"
AX, AY = 254.0, 228.6
# J9: USB 2.0 only (D+/D- pairs joined), SBU unused.
usb_nets = {"A4": "USB_VBUS", "A9": "USB_VBUS", "B4": "USB_VBUS", "B9": "USB_VBUS",
            "A5": "USB_CC1", "B5": "USB_CC2", "A7": "SC_IO19", "B7": "SC_IO19",
            "A6": "SC_IO20", "B6": "SC_IO20", "A8": None, "B8": None,
            "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH": "GND"}
place("Connector:USB_C_Receptacle_USB2.0_16P", "J9", "USB4105-GF-A",
      "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal", AX, AY, usb_pins, usb_nets,
      "Sidecar USB (USB-Serial-JTAG), USB-C, USB 2.0")
# CC1/CC2: 5.1k to GND each (a sink), so C-to-C cables connect and give 5 V.
place("Device:R", "R23", "5.1k", R_FP, AX + 30.48, AY - 10.16, res_pins, {"1": "USB_CC1", "2": "GND"})
place("Device:R", "R24", "5.1k", R_FP, AX + 38.1, AY - 10.16, res_pins, {"1": "USB_CC2", "2": "GND"})
# D3: VBUS to the board rail through a Schottky, so USB alone powers the
# board but nothing on the rail reaches the host. (With J6 fitted the rail is
# also the Atari's 5 V: pull J6 to run from USB, as for J4/J5.)
place("Device:D_Schottky", "D3", "B5819W", "Diode_SMD:D_SOD-123", AX + 30.48, AY + 7.62, schottky_pins,
      {"2": "USB_VBUS", "1": "+5V"}, "Schottky, 40 V 1 A")
# R22: the Sidecar's drive of the Atari's RESET (open drain), from IO3 (a
# strap only with the JTAG eFuse burned), 0 ohm as main's R1.
place("Device:R", "R22", "0", R_FP, AX + 53.34, AY - 10.16, res_pins, {"1": "SC_RESET_DRV", "2": "RESET"})
NOTE2 = ("sidecar_pcb_xe only: J9 USB-C for the Sidecar's USB (IO19 D-, IO20 D+; also on J7), CC1/CC2 5.1k "
         "to GND (R23, R24), VBUS through D3 (Schottky) to +5V: USB alone powers the board; pull J6 then, "
         "as for J4/J5, so the Atari isn't back-fed. R22 (0 ohm): Sidecar IO3 (SC_RESET_DRV) to RESET, open drain, "
         "beside main's R1; IO3 is no longer on J7 (pin 5 NC).")
add.append(f'\t(text "{NOTE2}"\n\t\t(exclude_from_sim no)\n\t\t(at {AX - 10:g} {AY + 33:g} 0)\n\t\t(effects\n'
           f'\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify left bottom)\n\t\t)\n'
           f'\t\t(uuid "{U()}")\n\t)\n')
out += add

k = s.index('\t(sheet_instances')
s = s[:k] + "".join(out) + s[k:]
open(dst, "w").write(s)
print(f"dropped J1 ({len(spots)} pins, {len(dropped)} labels); CART+ECI at {cx:g},{cy:g}")
for pin in sorted(xe_nets, key=lambda p: -cart_pins[p][1]):
    print(f"  ECI {pin:>4} -> {xe_nets[pin]}")
missing = sorted(set(xl_pbi.values()) - set(xe_nets.values()))
print("XL PBI nets without an ECI pin:", ", ".join(missing))
