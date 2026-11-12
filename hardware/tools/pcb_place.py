#!/usr/bin/env python3
"""Move and rotate footprints in a .kicad_pcb file by reference, in place.

KiCad stores a footprint's children (pads, texts, graphics) at positions
relative to the footprint and unrotated, but a pad's or text's angle field
is absolute. So moving is just the footprint's (at x y r); rotating by d
also adds d to every child angle. Graphics (fp_line etc.) have no angle.

    import pcb_place
    pcb = pcb_place.Pcb("board.kicad_pcb")
    pcb.place("J1", 88.9, 101.82, 0)
    pcb.save()
"""
import re


def _block(t, i):
    d = 0
    for j in range(i, len(t)):
        if t[j] == '(':
            d += 1
        elif t[j] == ')':
            d -= 1
            if d == 0:
                return t[i:j + 1]
    raise ValueError("unbalanced")


def _fmt(v):
    v = round(v, 4)
    return f"{v:g}"


class Pcb:
    def __init__(self, path):
        self.path = path
        self.s = open(path).read()

    def _find(self, ref):
        hits = [m.start() for m in re.finditer(r'\(property "Reference" "' + re.escape(ref) + '"', self.s)]
        if len(hits) != 1:
            raise KeyError(f"{ref}: {len(hits)} footprints")
        st = self.s.rindex('\n\t(footprint', 0, hits[0]) + 2
        return st, _block(self.s, st)

    def refs(self):
        return re.findall(r'\n\t\(footprint "[^"]+"(?:(?!\n\t\(footprint).)*?\(property "Reference" "([^"]+)"',
                          self.s, re.S)

    def get(self, ref):
        _, b = self._find(ref)
        x, y, r = re.search(r'\n\t\t\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', b).groups()
        return float(x), float(y), float(r or 0)

    def place(self, ref, x, y, rot=None):
        st, b = self._find(ref)
        ox, oy, orot = self.get(ref)
        rot = orot if rot is None else rot % 360
        d = (rot - orot) % 360
        hdr = re.search(r'\n\t\t\(at [^)]*\)', b)
        new_at = f'\n\t\t(at {_fmt(x)} {_fmt(y)}' + (f' {_fmt(rot)})' if rot else ')')
        nb = b[:hdr.start()] + new_at + b[hdr.end():]
        if d:
            # Children: pads, properties and fp_text carry absolute angles.
            def rot_child(m):
                cx, cy, ca = m.group(2), m.group(3), m.group(4)
                a = (float(ca or 0) + d) % 360
                return f'{m.group(1)}(at {cx} {cy}' + (f' {_fmt(a)})' if a else ')')
            head, body = nb[:nb.index(new_at) + len(new_at)], nb[nb.index(new_at) + len(new_at):]
            body = re.sub(r'(\((?:pad "[^"]*" \w+ \w+|property "[^"]*" "[^"]*"|fp_text \w+ "[^"]*")\s*)'
                          r'\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', rot_child, body)
            nb = head + body
        self.s = self.s[:st] + nb + self.s[st + len(b):]

    def delete(self, ref, only_unlinked=False):
        """Delete footprints with this reference (only those without a
        schematic path, if only_unlinked)."""
        n = 0
        for m in list(re.finditer(r'\n\t\(footprint "', self.s))[::-1]:
            b = _block(self.s, m.start() + 2)
            if f'(property "Reference" "{ref}"' not in b:
                continue
            if only_unlinked and re.search(r'\n\t\t\(path "', b):
                continue
            self.s = self.s[:m.start()] + self.s[m.start() + 2 + len(b):]
            n += 1
        return n

    def save(self, path=None):
        open(path or self.path, "w").write(self.s)
