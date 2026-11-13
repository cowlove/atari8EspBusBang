# Vendored KiCad symbols

The library symbols the schematic generators (`../tools/make_sidecar_schematic.py`,
`../tools/make_sidecar_xl_schematic.py`) embed, copied verbatim from KiCad
9.0.7's library ([kicad-symbols tag 9.0.7](https://gitlab.com/kicad/libraries/kicad-symbols/-/tree/9.0.7)),
with the parents of derived symbols. Only these symbols: each file is a
cut-down `<Library>.kicad_sym`.

They're here so the generated schematics don't depend on the installed
KiCad: KiCad 10's library changes some of them (the microSD socket loses a
pin, for one), which would change the generators' output. With these, both
generators reproduce the committed schematics byte for byte on KiCad 9 and
10. To move to newer symbols, replace these files deliberately and review
the regenerated schematics and boards.
