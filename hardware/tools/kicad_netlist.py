"""Read a `kicad-cli sch export netlist` file in one canonical layout.

The generators and netlist_to_pcb.py parse netlists with regexes written
against KiCad 9's compact layout ('(net (code "1") (name "/3V3") ...', a
node per line). KiCad 10 writes the same S-expressions pretty-printed, one
token per line. read() collapses every run of whitespace outside quoted
strings to one space and drops the space before a ')', so both versions read
alike ('(node (ref "R1") (pin "1") (pintype "passive"))'); strings are kept
exactly. Plain Python: no pcbnew needed.
"""
import re

# A quoted string (kept), whitespace before a ')' (dropped), other whitespace
# (one space).
_TOKENS = re.compile(r'("(?:[^"\\]|\\.)*")|(\s+(?=\)))|\s+')


def normalize(text):
    return _TOKENS.sub(lambda m: m.group(1) if m.group(1) is not None else ("" if m.group(2) else " "), text)


def read(path):
    with open(path) as f:
        return normalize(f.read())
