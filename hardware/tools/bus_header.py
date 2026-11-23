"""The bus header: the 2x25 connector between a Sidecar board and its Atari adapter.

One board design for every machine: the board carries a 2x25 2.54 mm socket
(J1) and a small adapter per machine carries the matching pin header plus
the machine's own connector:

    bus_adapter_pbi/  600XL/800XL: the PBI card-edge socket (the 50-way
                      5530843-5 the XL board used to carry)
    bus_adapter_eci/  130XE/65XE: the cartridge + ECI card edge (the eci2Pbi
                      adapter's fingers)
    sidecar_pcb_hdr/  the board (sidecar_pcb_xe with J1 swapped for the socket)

Pinout: header pin n is PBI pin n for every signal (odd pins one row, even
the other, as on the card edge), so the PBI adapter is a straight
pass-through and the board keeps the XL board's net per PBI pin. The PBI's
three NC pins (33, 37, 39) become GND on the header: 12 grounds instead of
9, one beside PHI2/RESET (33) and two between READY, EXTDEC and REFRESH.
The PBI adapter leaves the Atari's NC pins open; only the header's are
grounded.

EXTDEC, CAS and RAS stay on the header (pass-through from the PBI, open on
the ECI adapter, which has no pin for them); nothing on the board uses them
today. Both 5 V pins (47, 48) are the Atari's 5 V on each adapter.

Imported by the generators (make_bus_adapters.py, make_sidecar_hdr_*.py),
so the pinout lives here only.
"""

# Header pin -> signal. Signal names are the PBI's (the XL schematic's net
# names, without the leading "/").
PINS = {
    1: "GND", 2: "EXTSEL",
    3: "A0", 4: "A1", 5: "A2", 6: "A3", 7: "A4", 8: "A5", 9: "A6", 10: "GND",
    11: "A7", 12: "A8", 13: "A9", 14: "A10", 15: "A11", 16: "A12", 17: "A13", 18: "A14",
    19: "GND", 20: "A15",
    21: "D0", 22: "D1", 23: "D2", 24: "D3", 25: "D4", 26: "D5", 27: "D6", 28: "D7",
    29: "GND", 30: "GND", 31: "PHI2", 32: "GND",
    33: "GND",       # PBI NC1
    34: "RESET", 35: "IRQ", 36: "READY",
    37: "GND",       # PBI NC2
    38: "EXTDEC",
    39: "GND",       # PBI NC3
    40: "REFRESH", 41: "CAS", 42: "GND", 43: "MPD", 44: "RAS", 45: "GND", 46: "READ",
    47: "VCC50", 48: "VCC50", 49: "AUDIO", 50: "GND",
}
assert sorted(PINS) == list(range(1, 51))

# The PBI's own NC pins, grounded on the header only.
PBI_NC = (33, 37, 39)

# Signals with no ECI/cartridge pin: open on the ECI adapter.
NOT_ON_ECI = ("EXTDEC", "CAS", "RAS")

# Footprints. The board's socket is SMD (insertion force goes to the
# adapter, which is cheap to replace); the adapters' headers are through-hole.
# Keying is still to choose (see the READMEs): a shrouded header needs a
# mate the shroud admits.
SOCKET_FP = "Connector_PinSocket_2.54mm:PinSocket_2x25_P2.54mm_Vertical_SMD"
HEADER_FP = "Connector_PinHeader_2.54mm:PinHeader_2x25_P2.54mm_Vertical"


def table():
    """The pinout as Markdown rows (odd | even), for the READMEs."""
    rows = ["| Pin | Signal | Pin | Signal |", "|---|---|---|---|"]
    for n in range(1, 51, 2):
        note = lambda p: PINS[p] + (" (PBI NC)" if p in PBI_NC else "")
        rows.append(f"| {n} | {note(n)} | {n + 1} | {note(n + 1)} |")
    return "\n".join(rows)


if __name__ == "__main__":
    print(table())
