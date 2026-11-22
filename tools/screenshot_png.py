#!/usr/bin/env python3
"""Rebuild an Atari screen as a PNG from main's screenshot dump.

main's shutdown report prints dumpScreenshot()'s records (main/screenshot.cpp
has the format): the ANTIC/GTIA registers, the display list, each mode
line's screen memory and the character set. This walks the decoded display
list ("dl:" records) and renders each mode line as ANTIC would, with the
registers' last values (no display list interrupts, no players/missiles,
no fine scrolling offsets).

  tools/screenshot_png.py <log> <out.png> [--tag TAG] [--index N]
                          [--font FILE] [--scale-y N]

<log>: any capture holding the records (a stash/ boot log; "-" reads stdin);
other lines are ignored, and the records may follow a prefix on their line.
With several dumps the last is used (--index counts from 0, negative from the
end; --tag picks by its begin tag). --font: the character set when the dump
has none readable (the Atari's own OS ROM font, "--" in the dump): a 1 KB
font, or an OS ROM image, which is taken to end at $FFFF, read at CHBASE.
The palette is an NTSC approximation.

Exit codes: 0 written, 1 no dump found, 2 usage.
"""
import argparse
import math
import re
import struct
import sys
import zlib

RECORD = re.compile(r'(?:^|\s)(screenshot|antic|gtia|shadow|dlist|dl|screenmem|charset): (.*)$')


def hexbytes(fields):
    """Byte values of hex fields; "--" (not served by the board) reads 0."""
    out, unread = [], 0
    for f in fields:
        if f == '--':
            out.append(0)
            unread += 1
        else:
            out.append(int(f, 16))
    return out, unread


def parse(lines):
    """The dumps in the capture, in order: dicts of the parsed records."""
    shots, cur = [], None
    for line in lines:
        m = RECORD.search(line.rstrip('\r\n'))
        if not m:
            continue
        kind, rest = m.group(1), m.group(2).split()
        if kind == 'screenshot':
            if rest and rest[0] == 'begin':
                cur = {'tag': rest[1] if len(rest) > 1 else '', 'regs': {}, 'dl': [], 'charset': {},
                       'unread': 0}
            elif rest and rest[0] == 'end' and cur is not None:
                shots.append(cur)
                cur = None
            continue
        if cur is None:
            continue
        if kind in ('antic', 'gtia', 'shadow'):
            regs = cur['regs'].setdefault(kind, {})
            i = 0
            while i < len(rest):
                name = rest[i]
                i += 1
                vals = []
                while i < len(rest) and re.match(r'^(?:[0-9a-f]{2}|[0-9a-f]{4})$', rest[i]):   # names never look like that
                    vals.append(int(rest[i], 16))
                    i += 1
                regs[name] = vals[0] if len(vals) == 1 else vals
        elif kind == 'dl':
            cur['dl'].append(rest)
        elif kind == 'screenmem':
            data, unread = hexbytes(rest[1:])
            cur['unread'] += unread
            # The data of the mode line just before it.
            if cur['dl'] and cur['dl'][-1][2] == 'mode':
                cur['dl'][-1] = cur['dl'][-1] + ['@data', data]
        elif kind == 'charset':
            addr = int(rest[0], 16)
            data, unread = hexbytes(rest[1:])
            for k, v in enumerate(data):
                cur['charset'][addr + k] = None if rest[1 + k] == '--' else v
    return shots


def ntsc_rgb(c):
    """An approximate NTSC color for an Atari color byte (hue high nibble,
    luminance bits 1-3)."""
    hue, lum = c >> 4, (c & 0x0e) >> 1
    y = 0.08 + lum * 0.13
    if hue == 0:
        i = q = 0.0
    else:
        a = math.radians(-58 + (hue - 1) * 25.7)
        i, q = 0.22 * math.cos(a), 0.22 * math.sin(a)
    r = y + 0.956 * i + 0.621 * q
    g = y - 0.272 * i - 0.647 * q
    b = y - 1.106 * i + 1.703 * q
    return tuple(max(0, min(255, int(round(v * 255)))) for v in (r, g, b))


def write_png(path, width, height, rows):
    """rows: height lists of width (r, g, b) tuples."""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for r, g, b in row:
            raw += bytes((r, g, b))

    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


class Font:
    """Glyph rows by address: the dump's charset, else --font."""

    def __init__(self, dumped, font_file):
        self.dumped = dumped
        self.file = None
        if font_file:
            with open(font_file, 'rb') as f:
                self.file = f.read()
        self.missing = 0

    def byte(self, addr):
        v = self.dumped.get(addr)
        if v is not None:
            return v
        if self.file is not None:
            if len(self.file) == 1024:
                return self.file[addr & 0x3ff]
            off = addr - (0x10000 - len(self.file))   # a ROM image ending at $FFFF
            if 0 <= off < len(self.file):
                return self.file[off]
        self.missing += 1
        return 0


HIRES_PER_BYTE = {2: 8, 3: 8, 4: 8, 5: 8, 6: 16, 7: 16, 8: 32, 9: 32, 0xa: 16, 0xb: 16, 0xc: 16, 0xd: 8, 0xe: 8,
                  0xf: 8}
IMAGE_WIDTH = 384   # a wide playfield, in hires pixels (half color clocks)


def render(shot, font_file):
    antic = shot['regs'].get('antic', {})
    gtia = shot['regs'].get('gtia', {})
    colpf = gtia.get('colpf', [0, 0, 0, 0])
    colpm = gtia.get('colpm', [0, 0, 0, 0])
    colbk = gtia.get('colbk', 0)
    prior = gtia.get('prior', 0)
    chactl = antic.get('chactl', 0)
    chbase = antic.get('chbase', 0)
    font = Font(shot['charset'], font_file)
    bk = ntsc_rgb(colbk)
    pf = [ntsc_rgb(c) for c in colpf]
    hires_on = ntsc_rgb((colpf[2] & 0xf0) | (colpf[1] & 0x0e))   # modes 2, 3, F
    two_bit = [bk, pf[0], pf[1], pf[2]]
    rows = []

    def blank(n):
        for _ in range(n):
            rows.append([bk] * IMAGE_WIDTH)

    def glyph_row(base, code, r):
        return font.byte(base + code * 8 + (7 - r if chactl & 4 else r))

    for rec in shot['dl']:
        what = rec[2]
        if what == 'blank':
            blank(int(rec[3]))
            continue
        if what == 'jmp':
            blank(1)
            continue
        if what in ('jvb', 'end') or rec[1] == 'end':
            break
        if what != 'mode':
            continue
        mode = int(rec[3], 16)
        lines = int(rec[5])
        data = rec[rec.index('@data') + 1] if '@data' in rec else []
        width = len(data) * HIRES_PER_BYTE.get(mode, 8)
        x0 = max(0, (IMAGE_WIDTH - width) // 2)
        for line in range(lines):
            row = [bk] * IMAGE_WIDTH
            px = []   # this scan line's playfield, in hires pixels
            for b in data:
                if mode in (2, 3):
                    code, inv = b & 0x7f, b & 0x80
                    if mode == 3:
                        # 10 scan lines: rows 0-7, then two blank; the last
                        # quarter of the set (descenders): two blank, rows
                        # 2-7, then rows 0-1.
                        if code >= 0x60:
                            fr = None if line < 2 else line if line < 8 else line - 8
                        else:
                            fr = line if line < 8 else None
                        bits = glyph_row((chbase & 0xfc) << 8, code, fr) if fr is not None else 0
                    else:
                        bits = glyph_row((chbase & 0xfc) << 8, code, line)
                    if inv and chactl & 1:
                        bits = 0
                    if inv and chactl & 2:
                        bits ^= 0xff
                    px += [hires_on if bits & (0x80 >> k) else pf[2] for k in range(8)]
                elif mode in (4, 5):
                    code = b & 0x7f
                    bits = glyph_row((chbase & 0xfc) << 8, code, line if mode == 4 else line // 2)
                    colors = [bk, pf[0], pf[1], pf[3] if b & 0x80 else pf[2]]
                    for k in range(4):
                        c = colors[(bits >> (6 - 2 * k)) & 3]
                        px += [c, c]
                elif mode in (6, 7):
                    code = b & 0x3f
                    bits = glyph_row((chbase & 0xfe) << 8, code, line if mode == 6 else line // 2)
                    on = pf[b >> 6]
                    for k in range(8):
                        c = on if bits & (0x80 >> k) else bk
                        px += [c, c]
                elif mode in (8, 0xa, 0xd, 0xe):
                    each = {8: 8, 0xa: 4, 0xd: 2, 0xe: 2}[mode]
                    for k in range(4):
                        px += [two_bit[(b >> (6 - 2 * k)) & 3]] * each
                elif mode in (9, 0xb, 0xc):
                    each = {9: 4, 0xb: 2, 0xc: 2}[mode]
                    for k in range(8):
                        px += [pf[0] if b & (0x80 >> k) else bk] * each
                elif mode == 0xf:
                    gmode = prior >> 6
                    if gmode == 0:
                        px += [hires_on if b & (0x80 >> k) else pf[2] for k in range(8)]
                    else:
                        for nib in (b >> 4, b & 0xf):
                            if gmode == 1:     # 16 luminances of COLBK's hue
                                c = ntsc_rgb((colbk & 0xf0) | (nib & 0x0e))
                            elif gmode == 3:   # 16 hues at COLBK's luminance
                                c = ntsc_rgb((nib << 4) | (colbk & 0x0e))
                            else:              # 9 colors from the registers
                                regs = colpm + colpf + [colbk]
                                c = ntsc_rgb(regs[nib] if nib < 9 else colbk)
                            px += [c] * 4
            row[x0:x0 + len(px)] = px[:IMAGE_WIDTH - x0]
            rows.append(row)
    if not rows:
        blank(1)
    return rows, font.missing


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('log')
    ap.add_argument('png')
    ap.add_argument('--tag')
    ap.add_argument('--index', type=int, default=-1)
    ap.add_argument('--font')
    ap.add_argument('--scale-y', type=int, default=2, help='output rows per scan line (default 2: square-ish pixels)')
    a = ap.parse_args()
    src = sys.stdin if a.log == '-' else open(a.log, errors='replace')
    shots = parse(src)
    if a.tag is not None:
        shots = [s for s in shots if s['tag'] == a.tag]
    if not shots:
        print('screenshot_png: no screenshot dump found', file=sys.stderr)
        return 1
    try:
        shot = shots[a.index]
    except IndexError:
        print('screenshot_png: no dump %d (%d found)' % (a.index, len(shots)), file=sys.stderr)
        return 1
    rows, missing = render(shot, a.font)
    out = [row for row in rows for _ in range(max(1, a.scale_y))]
    write_png(a.png, IMAGE_WIDTH, len(out), out)
    print('screenshot_png: %s: %d scan lines from dump "%s"%s%s' % (
        a.png, len(rows), shot['tag'],
        ', %d screen bytes not served by the board (drawn as 0)' % shot['unread'] if shot['unread'] else '',
        ', %d font bytes missing (try --font)' % missing if missing else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
