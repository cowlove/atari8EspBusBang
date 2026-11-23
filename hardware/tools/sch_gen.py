"""Small helpers for the generated schematics (KiCad 9 format, 20250114).

Used by make_bus_adapters.py and make_sidecar_hdr_schematic.py. The older
generators (make_sidecar_*_schematic.py) carry their own copies of these;
they must keep reproducing their committed files byte for byte, so they
weren't moved over.
"""
import re
import uuid

PIN_RE = (r'\(pin \w+ \w+\s*\(at ([-\d.]+) ([-\d.]+) (\d+)\)\s*\(length [\d.]+\)'
          r'(?:\s*\(hide yes\))?.*?\(name "([^"]*)".*?\(number "([^"]*)"')
NET_RE = r'\(net \(code "\d+"\) \(name "([^"]+)"\)(.*?)\)\s*(?=\(net |\)\s*\)\s*$)'


def block(text, start):
    """The balanced (...) starting at text[start]."""
    d = 0
    for j in range(start, len(text)):
        if text[j] == '(':
            d += 1
        elif text[j] == ')':
            d -= 1
            if d == 0:
                return text[start:j + 1]
    raise ValueError("unbalanced")


class Uuids:
    """Deterministic UUID5s under one namespace string: reruns only differ where the circuit does."""

    def __init__(self, namespace):
        self.namespace, self.n = namespace, 0

    def __call__(self):
        self.n += 1
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"{self.namespace}/{self.n}"))


def lib_from_sch(path, name):
    """An embedded library symbol (lib:name) copied from another schematic, at lib_symbols depth."""
    t = open(path).read()
    return block(t, t.index(f'(symbol "{name}"'))


def lib_from_symdir(symdir, libfile, name):
    """A (non-derived) symbol from the vendored library, renamed libfile:name, at lib_symbols depth."""
    t = open(f"{symdir}/{libfile}.kicad_sym").read()
    b = block(t, t.index(f'\t(symbol "{name}"') + 1)
    assert "(extends" not in b[:400], "derived symbols: see make_sidecar_xl_schematic.py"
    b = b.replace(f'(symbol "{name}"', f'(symbol "{libfile}:{name}"', 1)
    return b.replace("\n", "\n\t")


def pins(lib_block):
    """Pin number -> (x, y, angle, name), symbol coordinates (y up)."""
    return {num: (float(x), float(y), int(a), nm) for x, y, a, nm, num in re.findall(PIN_RE, lib_block, re.S)}


def prop(name, value, x, y, hide=False):
    h = "\n\t\t\t\t(hide yes)" if hide else ""
    return (f'\t\t(property "{name}" "{value}"\n\t\t\t(at {x:g} {y:g} 0)\n\t\t\t(effects\n'
            f'\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t){h}\n\t\t\t)\n\t\t)\n')


def symbol(U, root, lib_id, ref, value, footprint, description, x, y, pin_numbers):
    """A placed symbol, no rotation."""
    pins_txt = "".join(f'\t\t(pin "{p}"\n\t\t\t(uuid "{U()}")\n\t\t)\n' for p in pin_numbers)
    return (f'\t(symbol\n\t\t(lib_id "{lib_id}")\n\t\t(at {x:g} {y:g} 0)\n\t\t(unit 1)\n'
            f'\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(dnp no)\n'
            f'\t\t(uuid "{U()}")\n'
            + prop("Reference", ref, x + 2.54, y - 3.81) + prop("Value", value, x + 2.54, y - 1.27)
            + prop("Footprint", footprint, x, y, True) + prop("Datasheet", "~", x, y, True)
            + prop("Description", description, x, y, True) + pins_txt
            + f'\t\t(instances\n\t\t\t(project ""\n\t\t\t\t(path "/{root}"\n'
              f'\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n')


# A label's angle and justification for a pin pointing at `angle`: the
# label reads away from the symbol body.
_LABEL_ANGLE = {180: 0, 0: 180, 270: 90, 90: 270}


def label(U, name, x, y, pin_angle):
    a = _LABEL_ANGLE[pin_angle]
    return (f'\t(label "{name}"\n\t\t(at {round(x, 4):g} {round(y, 4):g} {a})\n\t\t(effects\n\t\t\t(font\n'
            f'\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify {"left" if a in (0, 90) else "right"} bottom)\n'
            f'\t\t)\n\t\t(uuid "{U()}")\n\t)\n')


def no_connect(U, x, y):
    return f'\t(no_connect\n\t\t(at {round(x, 4):g} {round(y, 4):g})\n\t\t(uuid "{U()}")\n\t)\n'


def text(U, body, x, y):
    return (f'\t(text "{body}"\n\t\t(exclude_from_sim no)\n\t\t(at {x:g} {y:g} 0)\n\t\t(effects\n'
            f'\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify left bottom)\n\t\t)\n'
            f'\t\t(uuid "{U()}")\n\t)\n')


def pin_labels(U, x, y, pinmap, nets):
    """A label on each pin in nets (net None: a no-connect flag)."""
    out = []
    for num in sorted(nets, key=lambda p: (-pinmap[p][1], pinmap[p][0])):
        px, py, ang, _ = pinmap[num]
        if nets[num] is None:
            out.append(no_connect(U, x + px, y - py))
        else:
            out.append(label(U, nets[num], x + px, y - py, ang))
    return out


def new_schematic(root, libs, items, paper="A4"):
    """A whole one-sheet schematic; root is its UUID (also each symbol()'s instance path)."""
    lib_txt = "".join("\t\t" + b + "\n" for b in libs)
    return (f'(kicad_sch\n\t(version 20250114)\n\t(generator "eeschema")\n\t(generator_version "9.0")\n'
            f'\t(uuid "{root}")\n\t(paper "{paper}")\n\t(lib_symbols\n{lib_txt}\t)\n'
            + "".join(items)
            + '\t(sheet_instances\n\t\t(path "/"\n\t\t\t(page "1")\n\t\t)\n\t)\n\t(embedded_fonts no)\n)\n')


def pin_nets(netlist_text, ref):
    """Pin -> net name for one component, from a netlist's text (kicad_netlist.read())."""
    out = {}
    for name, body in re.findall(NET_RE, netlist_text, re.S):
        for pin in re.findall(r'\(node \(ref "' + re.escape(ref) + r'"\) \(pin "([^"]+)"\)', body):
            out[pin] = name
    return out
