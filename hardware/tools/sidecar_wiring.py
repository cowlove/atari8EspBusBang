#!/usr/bin/env python3
"""Draw the main-board <-> Sidecar wiring as docs/sidecar_wiring.pdf, and
the bus-sampler-only variant (bare main board, no link) as
docs/sidecar_sampler_wiring.pdf.

WIRES below is the one place the drawing comes from: change a row, rerun
this script (python3 hardware/tools/sidecar_wiring.py), commit both files. No Python
packages needed: the script writes PostScript and runs ps2pdf (Ghostscript).

Header positions are the ESP32-S3-DevKitC-1's J1/J3 pinout; check a clone's
silkscreen. The firmware pin assignments these rows must agree with:
  main/pinDefs.h                    host link pins (sidecarTxPin, ...)
  sidecar/main/sidecarApp.cpp       sidecarLinkConfig
The "planned" profiler rows are the Sidecar app's future bus-profiler
inputs (components/bus_profiler on the Sidecar's free core): the
pin map lives here until that code exists.
"""

import datetime
import os
import subprocess
import sys

# ESP32-S3-DevKitC-1 headers, pin 1 at the top (the end away from USB).
J1 = ["3V3", "3V3", "RST", 4, 5, 6, 7, 15, 16, 17, 18, 8, 3, 46, 9, 10, 11,
      12, 13, 14, "5V", "GND"]
J3 = ["GND", 43, 44, 1, 2, 42, 41, 40, 39, 38, 37, 36, 35, 0, 45, 48, 47, 21,
      20, 19, "GND", "GND"]

# Annotations printed beside a pin (both boards), for pins with fixed roles.
PIN_NOTES = {
    "host": {0: "strap", 3: "strap", 45: "strap", 46: "strap", 19: "USB D-",
             20: "USB D+", 43: "U0TXD", 44: "U0RXD"},
    "sidecar": {0: "strap", 3: "strap", 45: "strap", 46: "strap", 19: "USB D-",
                20: "USB D+", 43: "U0TXD", 44: "U0RXD", 35: "oPSRAM",
                36: "oPSRAM", 37: "oPSRAM"},
}

# (label, from, to, color RGB, status, note)
# from/to: ("host", gpio), ("sidecar", gpio), ("atari", "name") or
# ("sd", "pin name") for the microSD socket block.
# status: "wired" (in use now) or "planned".
SD_COLOR = (0.25, 0.25, 0.60)

WIRES = [
    ("Link TX",  ("host", 0),     ("sidecar", 9),  (0.85, 0.10, 0.10), "wired",
     "host TX -> Sidecar RX; also main IO0 (BOOT) for serial flashing; PBI audio on espBusBang only"),
    ("Link RX",  ("sidecar", 46), ("host", 21),    (0.10, 0.30, 0.90), "wired",
     "Sidecar TX -> host RX; Sidecar carrier: via jumper J3 2-3 (1-2: GPIO21 = RFSH)"),
    ("BCLK",     ("host", 48),    ("sidecar", 10), (0.05, 0.60, 0.15), "wired",
     "host drives the link clock; sampler (link off): main's profile pin GPIO48"),
    ("phi2",     ("host", 36),    ("sidecar", 4),  (0.95, 0.55, 0.00), "planned",
     "Sidecar bus profiler: clock"),
    ("R/W",      ("host", 17),    ("sidecar", 5),  (0.55, 0.20, 0.75), "planned",
     "Sidecar bus profiler; host R/W is GPIO15 on the Sidecar carrier (R/W regroup)"),
    ("extSel_",  ("host", 47),    ("sidecar", 6),  (0.55, 0.35, 0.15), "planned",
     "Sidecar bus profiler; open drain on the host, keep it short"),
    ("D0",       ("host", 38),    ("sidecar", 7),  (0.00, 0.60, 0.60), "planned",
     "Sidecar bus profiler"),
    ("D1",       ("host", 39),    ("sidecar", 15), (0.90, 0.20, 0.60), "planned",
     "Sidecar bus profiler"),
    ("A0",       ("host", 1),     ("sidecar", 16), (0.50, 0.55, 0.05), "planned",
     "Sidecar bus profiler"),
    ("RFSH",     ("atari", "RFSH (3.3 V side)"), ("sidecar", 17),
     (0.15, 0.15, 0.15), "planned",
     "Sidecar bus profiler refresh_; 3.3 V side, never the 5 V bus line; "
     "or main GPIO21 where it is refresh_ (espBusBang, .5)"),
    # microSD, 4-bit SDMMC (sidecar/main/sidecarApp.cpp, kSd*): six adjacent
    # pins (J3-4..9) in the socket's own pin order, so the wires don't cross.
    ("SD DAT2",  ("sidecar", 1),  ("sd", "DAT2"),  SD_COLOR, "planned", "10 k pull-up"),
    ("SD DAT3",  ("sidecar", 2),  ("sd", "DAT3"),  SD_COLOR, "planned", "10 k pull-up (also CD)"),
    ("SD CMD",   ("sidecar", 42), ("sd", "CMD"),   SD_COLOR, "planned", "10 k pull-up"),
    ("SD CLK",   ("sidecar", 41), ("sd", "CLK"),   SD_COLOR, "planned",
     "up to 40 MHz; keep it short, no pull-up"),
    ("SD DAT0",  ("sidecar", 40), ("sd", "DAT0"),  SD_COLOR, "planned", "10 k pull-up"),
    ("SD DAT1",  ("sidecar", 39), ("sd", "DAT1"),  SD_COLOR, "planned", "10 k pull-up"),
    # Serial flashing of the main board by the Sidecar (Atari off): EN, and
    # the Sidecar's UART to the main board's UART0, which is Atari D6/D5.
    ("Main EN",  ("sidecar", 11), ("host", "RST"), (0.30, 0.30, 0.30), "planned",
     "open drain: the Sidecar resets the main board (flashing, recovery)"),
    ("Flash TX", ("sidecar", 12), ("host", 44), (0.00, 0.45, 0.35), "planned",
     "via 1 k to main RXD0 = Atari D6: only with the Atari off"),
    ("Flash RX", ("host", 43), ("sidecar", 13), (0.60, 0.30, 0.00), "planned",
     "via 1 k from main TXD0 = Atari D5: only with the Atari off"),
    # Audio co-processor: PWM / sigma-delta out to the PBI audio input.
    ("Audio",    ("sidecar", 47), ("atari", "AUDIO (PBI 49)"), (0.85, 0.45, 0.70), "planned",
     "via 1 k series resistor (sets the level); host GPIO0 no longer on AUDIO"),
]

TITLE = "Main board <-> Sidecar wiring (ESP32-S3-DevKitC-1 pair)"

# A Sidecar as a bus sampler beside a bare test-pool main board (no link):
# the bus_profiler's 8 input bits, in its bit order, plus ground. Bit 0
# phi2, 1 profile pin, 2 R/W, 3 extSel_, 4 D0, 5 D1, 6 A0, 7 refresh_.
SAMPLER_TITLE = "Sidecar bus sampler wiring (bare main board, no link)"
SAMPLER_WIRES = [
    ("phi2",     ("host", 36), ("sidecar", 4),  (0.95, 0.55, 0.00), "wire", "bit 0: clock"),
    ("profile",  ("host", 48), ("sidecar", 10), (0.05, 0.60, 0.15), "wire", "bit 1: main's profile pin (BCLK wire when the link is used)"),
    ("R/W",      ("host", 17), ("sidecar", 5),  (0.55, 0.20, 0.75), "wire", "bit 2"),
    ("extSel_",  ("host", 47), ("sidecar", 6),  (0.55, 0.35, 0.15), "wire", "bit 3: open drain on the main board, keep it short"),
    ("D0",       ("host", 38), ("sidecar", 7),  (0.00, 0.60, 0.60), "wire", "bit 4"),
    ("D1",       ("host", 39), ("sidecar", 15), (0.90, 0.20, 0.60), "wire", "bit 5"),
    ("A0",       ("host", 1),  ("sidecar", 16), (0.50, 0.55, 0.05), "wire", "bit 6"),
    ("refresh_", ("host", 21), ("sidecar", 17), (0.15, 0.15, 0.15), "wire", "bit 7: main GPIO21 is refresh_ on this board"),
    ("GND",      ("host", "GND"), ("sidecar", "GND"), (0.0, 0.0, 0.0), "wire", "common ground: any GND pin on each board"),
]

PAGE_W, PAGE_H = 792, 612            # US letter, landscape
BOARD_W, PITCH, TOP = 200, 15, 535
BOARDS = {"host": 45, "sidecar": 450}   # left edge x
SD_X = BOARDS["sidecar"] + BOARD_W + 40   # the microSD block's left edge
SD_W = 62
PIN_INSET = 18


def pin_xy(board, gpio):
    x0 = BOARDS[board]
    for header, x in ((J1, x0 + PIN_INSET), (J3, x0 + BOARD_W - PIN_INSET)):
        if gpio in header:
            return x, TOP - 25 - header.index(gpio) * PITCH, header is J1
    raise SystemExit(f"GPIO{gpio} is not on a DevKitC-1 header")


def header_pos(gpio):
    for name, header in (("J1", J1), ("J3", J3)):
        if gpio in header:
            return f"{name}-{header.index(gpio) + 1}"
    return "?"


def ps_str(s):
    return "(" + str(s).replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)") + ")"


def draw(WIRES, TITLE, outname, subtitle, notes, banner=None):
    out = []
    w = out.append
    w("%!PS-Adobe-3.0")
    w(f"<< /PageSize [{PAGE_W} {PAGE_H}] >> setpagedevice")
    w("/F { /Helvetica findfont exch scalefont setfont } def")
    w("/FB { /Helvetica-Bold findfont exch scalefont setfont } def")
    w("/rshow { dup stringwidth pop neg 0 rmoveto show } def")
    w("/dot { newpath 0 360 arc fill } def")
    w("/ring { newpath 0 360 arc stroke } def")

    w(f"16 FB 40 {PAGE_H - 30} moveto {ps_str(TITLE)} show")
    stamp = datetime.date.today().isoformat()
    w(f"8 F 40 {PAGE_H - 44} moveto "
      f"{ps_str('Generated ' + stamp + ' by hardware/tools/sidecar_wiring.py. ' + subtitle + ' '
                'Header pin numbers: DevKitC-1, pin 1 away from the USB end.')} show")
    if banner:
        w(f"0.8 0 0 setrgbcolor 8.5 FB 40 {PAGE_H - 56} moveto {ps_str(banner)} show 0 setgray")

    used = {}
    for label, a, b, color, status, note in WIRES:
        for end in (a, b):
            if end[0] in ("host", "sidecar"):
                used[(end[0], end[1])] = color

    # Boards and pins.
    for board, x0 in BOARDS.items():
        bottom = TOP - 25 - 21 * PITCH - 15
        w("0.6 setlinewidth 0 setgray")
        w(f"{x0} {bottom} {BOARD_W} {TOP - bottom} rectstroke")
        name = "Main board (host app)" if board == "host" else "Sidecar board"
        w(f"11 FB {x0 + BOARD_W / 2} {TOP + 8} moveto {ps_str(name)} "
          "dup stringwidth pop 2 div neg 0 rmoveto show")
        w(f"7 F {x0 + 4} {bottom + 4} moveto (J1) show "
          f"{x0 + BOARD_W - 4} {bottom + 4} moveto (J3) rshow "
          f"{x0 + BOARD_W / 2} {bottom + 4} moveto (USB end) "
          "dup stringwidth pop 2 div neg 0 rmoveto show")
        for header, left in ((J1, True), (J3, False)):
            for i, pin in enumerate(header):
                x = x0 + PIN_INSET if left else x0 + BOARD_W - PIN_INSET
                y = TOP - 25 - i * PITCH
                text = f"GPIO{pin}" if isinstance(pin, int) else pin
                note = PIN_NOTES[board].get(pin, "") if isinstance(pin, int) else ""
                color = used.get((board, pin))
                if color:
                    w(f"{color[0]} {color[1]} {color[2]} setrgbcolor {x} {y} 3.2 dot")
                    w("7 FB")
                else:
                    w(f"0.55 setgray 0.5 setlinewidth {x} {y} 2.6 ring")
                    w("7 F")
                w("0 setgray")
                if left:
                    w(f"{x + 6} {y - 2.5} moveto {ps_str(text)} show")
                    if note:
                        w(f"6 F 0.4 setgray {x + 40} {y - 2.5} moveto {ps_str(note)} show 0 setgray")
                    w(f"5.5 F 0.5 setgray {x - 9} {y - 2} moveto ({i + 1}) rshow 0 setgray")
                else:
                    w(f"{x - 6} {y - 2.5} moveto {ps_str(text)} rshow")
                    if note:
                        w(f"6 F 0.4 setgray {x - 40} {y - 2.5} moveto {ps_str(note)} rshow 0 setgray")
                    w(f"5.5 F 0.5 setgray {x + 9} {y - 2} moveto ({i + 1}) show 0 setgray")

    # Wires.
    board_bottom = TOP - 25 - 21 * PITCH - 15
    gap_l = BOARDS["host"] + BOARD_W
    gap_r = BOARDS["sidecar"]
    atari_y = board_bottom + 30
    lane = 0
    sd_pins = []
    for label, a, b, color, status, note in WIRES:
        w(f"{color[0]} {color[1]} {color[2]} setrgbcolor 1.6 setlinewidth")
        w("[6 3] 0 setdash" if status == "planned" else "[] 0 setdash")
        w("newpath")
        if a[0] == "atari":
            bx, by, _ = pin_xy(*b)
            sx = (gap_l + gap_r) / 2
            w("[] 0 setdash 0.8 setlinewidth")
            w(f"{sx - 70} {atari_y - 9} 140 18 rectstroke")
            w(f"7 FB {sx} {atari_y - 2.5} moveto {ps_str('Atari ' + a[1])} "
              "dup stringwidth pop 2 div neg 0 rmoveto show")
            w("[6 3] 0 setdash 1.6 setlinewidth" if status == "planned" else "1.6 setlinewidth")
            w(f"{sx + 70} {atari_y} moveto {bx - 50} {atari_y} {bx - 60} {by} "
              f"{bx} {by} curveto stroke")
            continue
        if b[0] == "atari":
            # A Sidecar output to an Atari signal, from its J3 side: a box
            # to the right of the board, level with the pin.
            ax, ay, _ = pin_xy(*a)
            bx0, bw = SD_X - 15, 95
            w(f"{ax} {ay} moveto {bx0} {ay} lineto stroke")
            w("[] 0 setdash 0.8 setlinewidth")
            w(f"{bx0} {ay - 9} {bw} 18 rectstroke")
            w(f"7 FB {bx0 + bw / 2} {ay - 2.5} moveto {ps_str('Atari ' + b[1])} "
              "dup stringwidth pop 2 div neg 0 rmoveto show")
            continue
        if b[0] == "sd":
            ax, ay, _ = pin_xy(*a)
            w(f"{ax} {ay} moveto {SD_X} {ay} lineto stroke [] 0 setdash")
            sd_pins.append((b[1], ay))
            continue
        (ax, ay, aj1) = pin_xy(*a)
        (bx, by, bj1) = pin_xy(*b)
        # Order so the path runs host -> Sidecar.
        if a[0] == "sidecar":
            (ax, ay, aj1), (bx, by, bj1) = (bx, by, bj1), (ax, ay, aj1)
        if aj1:
            # Host J1 is on the far side: go out left, under the host board,
            # and into the gap.
            lane += 1
            ly = board_bottom - 8 * lane
            w(f"{ax} {ay} moveto {BOARDS['host'] - 12 - 4 * lane} {ay} lineto "
              f"{BOARDS['host'] - 12 - 4 * lane} {ly} lineto {gap_l + 20} {ly} lineto "
              f"{gap_l + 90} {ly} {bx - 80} {by} {bx} {by} curveto stroke")
        else:
            w(f"{ax} {ay} moveto {ax + 90} {ay} {bx - 90} {by} {bx} {by} curveto stroke")
        w("[] 0 setdash")
    w("[] 0 setdash")
    if sd_pins:
        top = max(y for _, y in sd_pins) + 30
        bot = min(y for _, y in sd_pins) - 26
        w(f"0 setgray 0.8 setlinewidth {SD_X} {bot} {SD_W} {top - bot} rectstroke")
        w(f"7 FB {SD_X + SD_W / 2} {top - 10} moveto (microSD) "
          "dup stringwidth pop 2 div neg 0 rmoveto show")
        w(f"6 F {SD_X + SD_W / 2} {top - 18} moveto (4-bit SDMMC) "
          "dup stringwidth pop 2 div neg 0 rmoveto show")
        for name, y in sd_pins:
            w(f"{SD_COLOR[0]} {SD_COLOR[1]} {SD_COLOR[2]} setrgbcolor {SD_X} {y} 2.6 dot")
            w(f"0 setgray 6.5 F {SD_X + 6} {y - 2.2} moveto {ps_str(name)} show")
        w(f"0.3 setgray 5.5 F {SD_X + 4} {bot + 12} moveto (VDD 3.3 V, GND) show "
          f"{SD_X + 4} {bot + 5} moveto (10 k pull-ups) show 0 setgray")

    # Table and legend: as many rows as fit under the drawing, the rest on
    # a second page.
    cols = [40, 100, 175, 255, 335, 400]
    row_h = 11.5

    def table_header(ty):
        w("0 setgray 8 FB")
        for x, h in zip(cols, ["Signal", "From", "To", "Header (from -> to)", "Status", "Notes"]):
            w(f"{x} {ty} moveto {ps_str(h)} show")
        w(f"0.5 setlinewidth [] 0 setdash {cols[0]} {ty - 3} moveto {PAGE_W - 40} {ty - 3} lineto stroke")

    def end_name(end):
        if end[0] == "sd":
            return "microSD " + end[1]
        if end[0] == "atari":
            return "Atari " + end[1].split()[0]
        pin = (f"GPIO{end[1]}" if isinstance(end[1], int)
               else end[1] if end[1] == "GND" else f"{end[1]} (EN)")
        return ("main " if end[0] == "host" else "Sidecar ") + pin

    def end_hdr(end):
        return "-" if end[0] in ("atari", "sd") else header_pos(end[1])

    legend = ("Solid: wired now. Dashed: planned. Filled pin: has a wire. "
              "oPSRAM: octal-PSRAM pin (N8R8, N16R8 modules).")

    ty = board_bottom - 40
    table_header(ty)
    y = ty - 14
    for label, a, b, color, status, note in WIRES:
        if y < 40:
            w("showpage")
            w(f"12 FB 40 {PAGE_H - 40} moveto {ps_str(TITLE + ' (continued)')} show")
            ty = PAGE_H - 70
            table_header(ty)
            y = ty - 14
        w(f"{color[0]} {color[1]} {color[2]} setrgbcolor 2 setlinewidth")
        w("[4 2] 0 setdash" if status == "planned" else "[] 0 setdash")
        w(f"{cols[0]} {y + 2.5} moveto {cols[0] + 16} {y + 2.5} lineto stroke [] 0 setdash")
        w("0 setgray 7.5 F")
        w(f"{cols[0] + 20} {y} moveto {ps_str(label)} show")
        w(f"{cols[1]} {y} moveto {ps_str(end_name(a))} show")
        w(f"{cols[2]} {y} moveto {ps_str(end_name(b))} show")
        w(f"{cols[3]} {y} moveto {ps_str(end_hdr(a) + ' -> ' + end_hdr(b))} show")
        w(f"{cols[4]} {y} moveto {ps_str(status)} show")
        w(f"{cols[5]} {y} moveto {ps_str(note)} show")
        y -= row_h
    w(f"0.3 setgray 7 F {cols[0]} {y - 4} moveto {ps_str(legend)} show")
    for i, line in enumerate(notes):
        w(f"0 setgray 8 FB {cols[0]} {y - 16 - 11 * i} moveto {ps_str(line)} show")
    w("showpage")

    # The drawing stays in docs/, where the firmware's comments point.
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs")
    ps_path = os.path.join(here, outname + ".ps")
    pdf_path = os.path.join(here, outname + ".pdf")
    with open(ps_path, "w") as f:
        f.write("\n".join(out) + "\n")
    try:
        subprocess.run(["ps2pdf", "-dEPSCrop", ps_path, pdf_path], check=True)
    finally:
        os.remove(ps_path)
    print(pdf_path)


TAP_NOTES = ["Bus-profiler taps: Sidecar inputs only, no pulls; 330-470 ohm in series at the main board end.",
             "Keep the Sidecar powered (its own USB) whenever the Atari is on: an unpowered ESP32 loads the bus lines."]


def main():
    draw(WIRES, TITLE, "sidecar_wiring",
         "Ground and power come through the carrier board (not drawn).", TAP_NOTES)
    draw(SAMPLER_WIRES, SAMPLER_TITLE, "sidecar_sampler_wiring",
         "Main board: a bare test-pool board (e.g. miner6 .5), Sidecar link off.",
         ["Wire ONLY these nine (8 signals + GND). Do NOT wire the link: main GPIO21 is refresh_ here, and the",
          "Sidecar's link TX (GPIO46) would fight the Atari's RFSH. Main GPIO48 is the profile pin with the link off."]
         + TAP_NOTES,
         banner="Wire ONLY these 8 signals + GND, 330-470 ohm in series. NO link wires: main GPIO21 is refresh_ here. "
                "Sidecar on USB power whenever the Atari is on.")


if __name__ == "__main__":
    sys.exit(main())
