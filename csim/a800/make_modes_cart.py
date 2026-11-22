#!/usr/bin/env python3
"""An 8 KB test cartridge (.car) showing one band of each ANTIC mode, for
the shutdown screenshot (main/screenshot.cpp, tools/screenshot_png.py).

Its display list ($A100), screen data ($A200 on) and character set ($B000,
a made-up font: a box with the code's bits inside) are all in the
cartridge ROM, as a classic cartridge game's would be, so the dump reads
them only when the board serves the cartridge (csim BUS_CYCLE=1, boot
config MODES_CSIM_BUS). The code sets the OS shadows (display list,
CHBAS, colors, DMA) and parks the 6502 in RAM ($0600: JMP $0600); the
OS's VBI copies the shadows to ANTIC and GTIA.

  csim/a800/make_modes_cart.py [out.car]     (default modes.car)
"""
import struct
import sys

BASE = 0xA000
rom = bytearray(8192)


def put(addr, data):
    rom[addr - BASE:addr - BASE + len(data)] = bytes(data)


# The bands: (mode, mode lines, bytes per line at normal width). 16 scan
# lines each.
BANDS = [(0x2, 2, 40), (0x4, 2, 40), (0x6, 2, 20), (0x7, 1, 20), (0x8, 2, 10), (0x9, 4, 10),
         (0xA, 4, 20), (0xB, 8, 20), (0xC, 16, 20), (0xD, 8, 40), (0xE, 16, 40), (0xF, 16, 40)]

dl = [0x70, 0x70, 0x70]
addr = 0xA200
for mode, lines, nbytes in BANDS:
    dl += [0x40 | mode, addr & 0xFF, addr >> 8] + [mode] * (lines - 1)
    data = []
    for i in range(lines * nbytes):
        if mode in (2, 4, 6, 7):
            data.append((0x21 + i) & 0xFF if mode in (2, 4) else (0x01 + i) & 0xFF)
        else:
            data.append((i * 0x1B + (i // nbytes) * 0x55) & 0xFF)
    put(addr, data)
    addr += len(data)
dl += [0x41, 0x00, 0xA1]   # JVB $A100
assert addr <= 0xB000, hex(addr)
put(0xA100, dl)

# The font: a box, the code's bits (1-6) across the middle rows.
font = []
for code in range(128):
    for r in range(8):
        font.append(0xFF if r in (0, 7) else 0x81 | ((code << 1) & 0x7E) if r % 2 else 0x81)
put(0xB000, font)

code = [
    0xA9, 0x00, 0x8D, 0x30, 0x02,   # LDA #<$A100, STA SDLSTL
    0xA9, 0xA1, 0x8D, 0x31, 0x02,   # LDA #>$A100, STA SDLSTH
    0xA9, 0xB0, 0x8D, 0xF4, 0x02,   # LDA #$B0, STA CHBAS
    0xA9, 0x28, 0x8D, 0xC4, 0x02,   # COLOR0 orange
    0xA9, 0xCA, 0x8D, 0xC5, 0x02,   # COLOR1 green
    0xA9, 0x94, 0x8D, 0xC6, 0x02,   # COLOR2 blue
    0xA9, 0x46, 0x8D, 0xC7, 0x02,   # COLOR3 red
    0xA9, 0x00, 0x8D, 0xC8, 0x02,   # COLOR4 black
    0xA9, 0x22, 0x8D, 0x2F, 0x02,   # SDMCTL: normal playfield, display list DMA
    0xA9, 0x4C, 0x8D, 0x00, 0x06,   # $0600: JMP $0600
    0xA9, 0x00, 0x8D, 0x01, 0x06,
    0xA9, 0x06, 0x8D, 0x02, 0x06,
    0x4C, 0x00, 0x06,               # JMP $0600
]
put(0xA000, code)
put(0xA0F0, [0x60])                  # the init routine: RTS
rom[0x1FFA:0x1FFC] = b"\x00\xa0"     # start address $A000
rom[0x1FFC] = 0x00                   # a cartridge is present
rom[0x1FFD] = 0x04                   # start it; no disk boot
rom[0x1FFE:0x2000] = b"\xf0\xa0"     # init address $A0F0
checksum = sum(rom) & 0xFFFFFFFF
header = b"CART" + struct.pack(">II", 1, checksum) + b"\0\0\0\0"   # type 1: standard 8 KB
out = sys.argv[1] if len(sys.argv) > 1 else "modes.car"
with open(out, "wb") as f:
    f.write(header + rom)
