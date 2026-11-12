#!/usr/bin/env python3
"""Generate the Sidecar carrier schematic from the espBusBang one.

Adds the Sidecar DevKitC (U2), its microSD header (J2) with pull-ups
(R2-R6), the audio resistor (R7) and the GPIO21 jumper header (J3), and
re-nets the main DevKitC (U1) as espBusBangSidecar/README.md describes,
with the inter-board wiring of docs/sidecar_wiring.py.

    python3 hardware/tools/make_sidecar_schematic.py \
        hardware/espBusBang/atariEspBusBang3.kicad_sch \
        hardware/espBusBangSidecar/espBusBangSidecar.kicad_sch

It rewrites the output from scratch: edit this script, not the generated
schematic, for anything it places (then check with kicad-cli sch erc and
sch export netlist). Layout-only work belongs in the PCB.
"""
import re
import sys
import uuid

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()


_uuid_count = 0


def U():
    """A UUID that is the same on every run, so regenerating the schematic
    only changes what the script changes."""
    global _uuid_count
    _uuid_count += 1
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"atari8EspBusBang/espBusBangSidecar/{_uuid_count}"))


def block(text, start):
    """The balanced (...) starting at index start."""
    d = 0
    for j in range(start, len(text)):
        if text[j] == '(':
            d += 1
        elif text[j] == ')':
            d -= 1
            if d == 0:
                return text[start:j + 1]
    raise ValueError("unbalanced")


ROOT = re.search(r'\(uuid "([^"]+)"\)', s).group(1)   # the sheet's uuid

# --- A3 paper: room for U2 and the SD header beside the original drawing.
s = s.replace('(paper "A4")', '(paper "A3")', 1)

# --- Split U1's nets that the link takes over. Each label is identified by
# its name and position (the U1 pin end, or R1's GPIO end).
renames = [
    ("AUDIO", "90.17 88.9 0", "LINK_TX"),       # U1 GPIO0
    ("REFRESH", "90.17 132.08 0", "U1_GPIO21"), # U1 GPIO21: J3's middle pin
    ("GPIO48", "90.17 152.4 0", "LINK_BCLK"),   # U1 GPIO48
    ("GPIO48", "142.24 110.49 0", "RESET_DRV"), # R1's ESP end, now unused
    # R/W regroup: R/W moves next to the page bits (A8-A13 on GPIO9-14), so
    # core 1 reads its 7-bit ctrl[] index with one extui; A14/A15 move up.
    ("A14", "39.37 78.74 180", "READ"),         # U1 GPIO15: was A14
    ("A15", "39.37 81.28 180", "A14"),          # U1 GPIO16: was A15
    ("READ", "90.17 127 0", "A15"),             # U1 GPIO17: was R/W
]
for old, at, new in renames:
    pat = f'(label "{old}"\n\t\t(at {at})'
    assert s.count(pat) == 1, (old, at, s.count(pat))
    s = s.replace(pat, f'(label "{new}"\n\t\t(at {at})')

# --- Pin table of the embedded DevKitC symbol (symbol coordinates, y up).
sym = block(s, s.index('(symbol "Espressif:ESP32-S3-DevKitC"'))
pins = {}
for m in re.finditer(r'\(pin \w+ \w+\s*\(at ([-\d.]+) ([-\d.]+) (\d+)\)\s*\(length [\d.]+\)'
                     r'.*?\(name "([^"]*)".*?\(number "([^"]*)"', sym, re.S):
    x, y, ang, name, num = m.groups()
    pins[num] = (float(x), float(y), int(ang), name)

# --- U2: a copy of U1's instance, moved, renamed, with fresh uuids.
u1_start = s.index('(lib_id "Espressif:ESP32-S3-DevKitC")')
u1_start = s.rindex('(symbol', 0, u1_start)
u1 = block(s, u1_start)
U2X, U2Y = 250.19, 111.76
dx, dy = U2X - 64.77, U2Y - 111.76


def shift_at(m):
    x, y, a = float(m.group(1)), float(m.group(2)), m.group(3)
    return f'(at {round(x + dx, 4):g} {round(y + dy, 4):g} {a})'


u2 = re.sub(r'\(at ([-\d.]+) ([-\d.]+) (\d+)\)', shift_at, u1)
u2 = re.sub(r'\(uuid "[^"]+"\)', lambda m: f'(uuid "{U()}")', u2)
u2 = u2.replace('(property "Reference" "U1"', '(property "Reference" "U2"')
u2 = u2.replace('(reference "U1")', '(reference "U2")')
u2 = u2.replace('(property "Value" "ESP32-S3-DevKitC"', '(property "Value" "ESP32-S3-DevKitC (Sidecar)"')

LABEL = '''\t(label "{name}"
\t\t(at {x:g} {y:g} {a})
\t\t(effects
\t\t\t(font
\t\t\t\t(size 1.27 1.27)
\t\t\t)
\t\t\t(justify {j} bottom)
\t\t)
\t\t(uuid "{u}")
\t)
'''


def label(name, x, y, a):
    j = "left" if a in (0, 90) else "right"
    return LABEL.format(name=name, x=round(x, 4), y=round(y, 4), a=a, j=j, u=U())


def pin_label(num, net, cx, cy):
    px, py, ang, _ = pins[num]
    x, y = cx + px, cy - py
    # Label angle from the side the pin leaves: right side 0, left 180,
    # top 90, bottom 270 (as the carrier's U1 labels are drawn).
    a = {180: 0, 0: 180, 270: 90, 90: 270}[ang]
    return label(net, x, y, a)


# Sidecar pin -> net (docs/sidecar_wiring.py). Pin numbers are the symbol's.
by_gpio = {}
for num, (_, _, _, name) in pins.items():
    m = re.match(r'GPIO(\d+)', name)
    if m:
        by_gpio[int(m.group(1))] = num
u2_nets = {
    # link
    by_gpio[9]: "LINK_TX", by_gpio[46]: "LINK_RX", by_gpio[10]: "LINK_BCLK",
    # bus profiler inputs
    by_gpio[4]: "PHI2", by_gpio[5]: "READ", by_gpio[6]: "EXTSEL",
    by_gpio[7]: "D0", by_gpio[15]: "D1", by_gpio[16]: "A0",
    by_gpio[17]: "REFRESH",
    # microSD, 4-bit SDMMC
    by_gpio[1]: "SD_DAT2", by_gpio[2]: "SD_DAT3", by_gpio[42]: "SD_CMD",
    by_gpio[41]: "SD_CLK", by_gpio[40]: "SD_DAT0", by_gpio[39]: "SD_DAT1",
    # audio out (PWM or sigma-delta) to the PBI audio input, through R7
    by_gpio[47]: "SC_AUDIO",
    # serial flashing of the main board (Atari off): EN, and UART1 to the
    # main board's UART0 (its D6/D5) through R12/R13; BOOT is LINK_TX
    by_gpio[11]: "MAIN_EN", by_gpio[12]: "SC_FLASH_TX", by_gpio[13]: "SC_FLASH_RX",
    # power: both boards from the carrier's 5 V; the Sidecar's own 3V3
    # feeds only its SD card (two DevKitC regulators never tied together)
    "1": "SC_3V3", "21": "VCC50_1", "22": "GND",
}
new = []
for num, net in u2_nets.items():
    new.append(pin_label(num, net, U2X, U2Y))
# The main board's EN (CHIP_PU, pin 3), for the Sidecar to reset it.
new.append(pin_label("3", "MAIN_EN", 64.77, 111.76))

# --- J2: an 8-pin header for a microSD breakout, in the card's pin order.
CONN = '''\t\t(symbol "Connector_Generic:Conn_01x08"
\t\t\t(pin_names
\t\t\t\t(offset 1.016)
\t\t\t\t(hide yes)
\t\t\t)
\t\t\t(exclude_from_sim no)
\t\t\t(in_bom yes)
\t\t\t(on_board yes)
\t\t\t(property "Reference" "J"
\t\t\t\t(at 0 10.16 0)
\t\t\t\t(effects
\t\t\t\t\t(font
\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t)
\t\t\t\t)
\t\t\t)
\t\t\t(property "Value" "Conn_01x08"
\t\t\t\t(at 0 -12.7 0)
\t\t\t\t(effects
\t\t\t\t\t(font
\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t)
\t\t\t\t)
\t\t\t)
\t\t\t(property "Footprint" ""
\t\t\t\t(at 0 0 0)
\t\t\t\t(effects
\t\t\t\t\t(font
\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t)
\t\t\t\t\t(hide yes)
\t\t\t\t)
\t\t\t)
\t\t\t(property "Datasheet" "~"
\t\t\t\t(at 0 0 0)
\t\t\t\t(effects
\t\t\t\t\t(font
\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t)
\t\t\t\t\t(hide yes)
\t\t\t\t)
\t\t\t)
\t\t\t(property "Description" "Generic connector, single row, 01x08"
\t\t\t\t(at 0 0 0)
\t\t\t\t(effects
\t\t\t\t\t(font
\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t)
\t\t\t\t\t(hide yes)
\t\t\t\t)
\t\t\t)
\t\t\t(symbol "Conn_01x08_1_1"
\t\t\t\t(rectangle
\t\t\t\t\t(start -1.27 8.89)
\t\t\t\t\t(end 1.27 -11.43)
\t\t\t\t\t(stroke
\t\t\t\t\t\t(width 0.254)
\t\t\t\t\t\t(type default)
\t\t\t\t\t)
\t\t\t\t\t(fill
\t\t\t\t\t\t(type background)
\t\t\t\t\t)
\t\t\t\t)
{pins}\t\t\t)
\t\t\t(embedded_fonts no)
\t\t)
'''
CPIN = '''\t\t\t\t(pin passive line
\t\t\t\t\t(at -5.08 {y:g} 0)
\t\t\t\t\t(length 3.81)
\t\t\t\t\t(name "Pin_{n}"
\t\t\t\t\t\t(effects
\t\t\t\t\t\t\t(font
\t\t\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t\t\t)
\t\t\t\t\t\t)
\t\t\t\t\t)
\t\t\t\t\t(number "{n}"
\t\t\t\t\t\t(effects
\t\t\t\t\t\t\t(font
\t\t\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t\t\t)
\t\t\t\t\t\t)
\t\t\t\t\t)
\t\t\t\t)
'''
def conn_symbol(n):
    """KiCad's Connector_Generic:Conn_01x<n>, pin 1 at the top."""
    top = 2.54 * ((n - 1) // 2)
    pins = "".join(CPIN.format(y=round(top - 2.54 * i, 2), n=i + 1) for i in range(n))
    t = CONN.format(pins=pins).replace("Conn_01x08", f"Conn_01x{n:02d}").replace("01x08", f"01x{n:02d}")
    t = t.replace("(start -1.27 8.89)", f"(start -1.27 {top + 1.27:g})")
    t = t.replace("(end 1.27 -11.43)", f"(end 1.27 {top - 2.54 * (n - 1) - 1.27:g})")
    t = t.replace("(at 0 10.16 0)", f"(at 0 {top + 2.54:g} 0)").replace("(at 0 -12.7 0)", f"(at 0 {top - 2.54 * n:g} 0)")
    return t
lib_end = s.index('\t(lib_symbols')
# Insert the connector definition just before lib_symbols' closing paren.
close_idx = lib_end + 1 + len(block(s, lib_end + 1)) - 1
s = s[:close_idx].rstrip('\t') + conn_symbol(8) + conn_symbol(3) + "\t" + s[close_idx:]


def prop(name, value, x, y, hide=False, angle=0):
    h = "\n\t\t\t\t(hide yes)" if hide else ""
    return f'''\t\t(property "{name}" "{value}"
\t\t\t(at {x:g} {y:g} {angle})
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t){h}
\t\t\t)
\t\t)
'''


def instance(lib_id, ref, value, footprint, x, y, angle, pin_numbers, refpos, valpos):
    pins_txt = "".join(f'\t\t(pin "{p}"\n\t\t\t(uuid "{U()}")\n\t\t)\n' for p in pin_numbers)
    return (f'\t(symbol\n\t\t(lib_id "{lib_id}")\n\t\t(at {x:g} {y:g} {angle})\n\t\t(unit 1)\n'
            f'\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)\n'
            f'\t\t(uuid "{U()}")\n'
            + prop("Reference", ref, *refpos)
            + prop("Value", value, *valpos)
            + prop("Footprint", footprint, x, y, hide=True)
            + prop("Datasheet", "~", x, y, hide=True)
            + prop("Description", "", x, y, hide=True)
            + pins_txt
            + f'\t\t(instances\n\t\t\t(project ""\n\t\t\t\t(path "/{ROOT}"\n'
              f'\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n')


# J2 at (340, 100): pin n's end is at (334.92, 92.38 + 2.54*(n-1)).
JX, JY = 340.36, 101.6
j2_nets = ["SD_DAT2", "SD_DAT3", "SD_CMD", "SC_3V3", "SD_CLK", "GND", "SD_DAT0", "SD_DAT1"]
new.append(instance("Connector_Generic:Conn_01x08", "J2", "microSD (SDMMC 4-bit)",
                    "Connector_PinHeader_2.54mm:PinHeader_1x08_P2.54mm_Vertical",
                    JX, JY, 0, [str(i + 1) for i in range(8)],
                    (JX, JY - 10.16), (JX, JY + 12.7)))
for i, net in enumerate(j2_nets):
    new.append(label(net, JX - 5.08, JY - 7.62 + 2.54 * i, 180))

# R2-R6: 10 k pull-ups to SC_3V3 on CMD and DAT0-3, vertical, pin 1 up.
RY = 140.97
for k, net in enumerate(["SD_CMD", "SD_DAT0", "SD_DAT1", "SD_DAT2", "SD_DAT3"]):
    rx = 314.96 + 10.16 * k
    new.append(instance("Device:R", f"R{k + 2}", "10k",
                        "Resistor_THT:R_Axial_DIN0309_L9.0mm_D3.2mm_P12.70mm_Horizontal",
                        rx, RY, 0, ["1", "2"], (rx + 2.54, RY - 1.27), (rx + 2.54, RY + 1.27)))
    new.append(label("SC_3V3", rx, RY - 3.81, 90))
    new.append(label(net, rx, RY + 3.81, 270))

# R7: series resistor from the Sidecar's audio pin to the PBI AUDIO input
# (J1.49); its value sets the level.
R7X, R7Y = 289.56, 160.02
new.append(instance("Device:R", "R7", "1k",
                    "Resistor_THT:R_Axial_DIN0309_L9.0mm_D3.2mm_P12.70mm_Horizontal",
                    R7X, R7Y, 0, ["1", "2"], (R7X + 2.54, R7Y - 1.27), (R7X + 2.54, R7Y + 1.27)))
new.append(label("SC_AUDIO", R7X, R7Y - 3.81, 90))
new.append(label("AUDIO", R7X, R7Y + 3.81, 270))

# R12/R13: 1 k in the Sidecar's flashing UART, to the main board's UART0,
# which is Atari D6 (RXD0, GPIO44) and D5 (TXD0, GPIO43): they isolate the
# Sidecar's stubs from the data bus and limit the current if the Sidecar
# drove TX with the Atari on.
for k, (sc_net, bus_net) in enumerate([("SC_FLASH_TX", "D6"), ("SC_FLASH_RX", "D5")]):
    rx = 299.72 + 10.16 * k
    new.append(instance("Device:R", f"R{12 + k}", "1k",
                        "Resistor_THT:R_Axial_DIN0309_L9.0mm_D3.2mm_P12.70mm_Horizontal",
                        rx, R7Y, 0, ["1", "2"], (rx + 2.54, R7Y - 1.27), (rx + 2.54, R7Y + 1.27)))
    new.append(label(sc_net, rx, R7Y - 3.81, 90))
    new.append(label(bus_net, rx, R7Y + 3.81, 270))

# J3: jumper header for U1 GPIO21. 1-2: GPIO21 reads the PBI RFSH line
# (the standard board); 2-3: GPIO21 is the link's RX from U2 GPIO46.
J3X, J3Y = 127.0, 132.08
new.append(instance("Connector_Generic:Conn_01x03", "J3", "GPIO21: 1-2 RFSH, 2-3 Sidecar",
                    "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical",
                    J3X, J3Y, 0, ["1", "2", "3"], (J3X, J3Y - 5.08), (J3X, J3Y + 5.08)))
for i, net in enumerate(["REFRESH", "U1_GPIO21", "LINK_RX"]):
    new.append(label(net, J3X - 5.08, J3Y - 2.54 + 2.54 * i, 180))

# A note on the sheet with the net changes.
NOTE = ("Sidecar (U2) wiring: docs/sidecar_wiring.pdf. Link: LINK_TX (U1 GPIO0 -> U2 GPIO9), "
        "LINK_RX (U2 GPIO46 -> J3 pin 3), LINK_BCLK (U1 GPIO48 -> U2 GPIO10).\\n"
        "J3 selects U1 GPIO21 (U1_GPIO21, pin 2): jumper 1-2 = PBI RFSH (REFRESH), 2-3 = Sidecar link RX (LINK_RX).\\n"
        "Changed from espBusBang: U1 GPIO0 no longer reaches the PBI bus; PBI AUDIO (J1.49) is driven by the Sidecar (U2 GPIO47, SC_AUDIO) through R7; "
        "U1 GPIO21 reaches RFSH only through J3; U1 GPIO48 left R1/RESET (RESET_DRV unconnected).\\n"
        "U1 R/W regroup: GPIO15 = R/W (READ), GPIO16 = A14, GPIO17 = A15 (was A14, A15, R/W): needs the firmware changes in README.md.\\n"
        "Serial flashing of U1 by U2 (Atari off): MAIN_EN (U2 GPIO11, open drain, to U1 EN), BOOT = LINK_TX "
        "(U2 GPIO9 to U1 IO0), SC_FLASH_TX (U2 GPIO12) -R12-> D6 = U1 RXD0, SC_FLASH_RX (U2 GPIO13) <-R13- D5 = U1 TXD0.\\n"
        "U2 profiler inputs: PHI2, READ, EXTSEL, D0, D1, A0, REFRESH. J2: microSD breakout, "
        "10k pull-ups R2-R6 to the Sidecar's own 3V3 (SC_3V3); U1 and U2 3V3 never tied.")
new.append(f'''\t(text "{NOTE}"
\t\t(exclude_from_sim no)
\t\t(at 210.82 172.72 0)
\t\t(effects
\t\t\t(font
\t\t\t\t(size 1.27 1.27)
\t\t\t)
\t\t\t(justify left bottom)
\t\t)
\t\t(uuid "{U()}")
\t)
''')

# Insert U2 and the new items before (sheet_instances.
k = s.index('\t(sheet_instances')
s = s[:k] + "\t" + u2 + "\n" + "".join(new) + s[k:]
open(dst, "w").write(s)
print("U2 nets:", len(u2_nets), "labels added:", sum(1 for t in new if t.startswith('\t(label')))
