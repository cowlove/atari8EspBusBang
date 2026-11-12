#!/usr/bin/env python3
"""PCBWay assembly files for one board: a BOM and a placement (CPL) file.

    assembly_files.py BOARD.kicad_pcb OUTDIR

Writes OUTDIR/<board>-bom.csv and OUTDIR/<board>-cpl.csv. Run by
export_pcbway.sh for the boards it assembles.

The schematics carry no manufacturer part numbers, so PARTS below maps each
(Value, Footprint) to one. Every placed part must match an entry (or be
listed in SKIP), or this fails: a new part needs a line here. Positions come
from `kicad-cli pcb export pos`, in the gerbers' absolute coordinates.
"""
import csv
import os
import subprocess
import sys
import tempfile

KICAD_CLI = os.environ.get("KICAD_CLI", "kicad-cli")

HDR = "Connector_PinHeader_2.54mm:PinHeader_{}_P2.54mm_Vertical"
C0805 = "Capacitor_SMD:C_0805_2012Metric_Pad1.18x1.45mm_HandSolder"
R0805 = "Resistor_SMD:R_0805_2012Metric_Pad1.20x1.40mm_HandSolder"
DEVKITC = "Espressif:ESP32-S2-DevKitC-1"

# (Value, Footprint) -> (manufacturer, MPN, description, package, notes)
PARTS = {
    ("10u", C0805): ("Samsung", "CL21A106KAYNNNE", "10uF 25V X5R MLCC", "0805", ""),
    ("22u", C0805): ("Samsung", "CL21A226MAQNNNE", "22uF 25V X5R MLCC", "0805", ""),
    ("100n", C0805): ("Samsung", "CL21B104KBCNNNC", "100nF 50V X7R MLCC", "0805", ""),
    ("1u", C0805): ("Samsung", "CL21B105KBFNNNE", "1uF 50V X7R MLCC", "0805", ""),
    ("10k", R0805): ("UNI-ROYAL", "0805W8F1002T5E", "10k 1% resistor", "0805", ""),
    ("1k", R0805): ("UNI-ROYAL", "0805W8F1001T5E", "1k 1% resistor", "0805", ""),
    # R1, RESET_DRV to the PBI RESET: the schematic value is just "R".
    ("R", R0805): ("UNI-ROYAL", "0805W8F0000T5E", "0 ohm resistor", "0805", ""),
    ("LED (MPD_ activity)", "LED_SMD:LED_0805_2012Metric_Pad1.15x1.40mm_HandSolder"):
        ("Hubei KENTO", "KT-0805G", "Green LED", "0805",
         "Any 0805 LED, colour free"),
    ("SK6812MINI", "LED_SMD:LED_SK6812MINI_PLCC4_3.5x3.5mm_P1.75mm"):
        ("OPSCO", "SK6812MINI", "RGB LED, integrated driver", "PLCC4 3.5x3.5mm", ""),
    ("ESP32-S3-WROOM-1-N8R2", "RF_Module:ESP32-S3-WROOM-1"):
        ("Espressif", "ESP32-S3-WROOM-1-N8R2", "ESP32-S3 module, 8MB flash, 2MB PSRAM", "SMD module", ""),
    ("ESP32-S3-WROOM-1-N16R8", "RF_Module:ESP32-S3-WROOM-1"):
        ("Espressif", "ESP32-S3-WROOM-1-N16R8", "ESP32-S3 module, 16MB flash, 8MB octal PSRAM", "SMD module", ""),
    ("AP7361C-33E", "Package_TO_SOT_SMD:SOT-223-3_TabPin2"):
        ("Diodes Inc", "AP7361C-33E-13", "3.3V 1A LDO regulator", "SOT-223", "Mounted on the bottom side"),
    ("microSD (Hirose DM3AT-SF-PEJM5)", "Connector_Card:microSD_HC_Hirose_DM3AT-SF-PEJM5"):
        ("Hirose", "DM3AT-SF-PEJM5", "microSD socket, push-push", "SMD", ""),
    ("5530843-5", "footprints:CONN50_5530843-5_TEC"):
        ("TE Connectivity", "5530843-5", "50-pos 2.54mm card edge connector (PBI)", "THT",
         "Insert from the BOTTOM side (opposite the modules), solder on top. "
         "The footprint and its silkscreen are on the top layer"),
}

# Parts whose footprint is on the top layer but go in from the bottom.
BOTTOM_INSERTED = {"footprints:CONN50_5530843-5_TEC"}

# Pin headers: generic male 2.54 mm, any make.
for size, pins in (("1x02", 2), ("1x03", 3), ("1x08", 8), ("2x03", 6), ("2x07", 14), ("2x08", 16)):
    PARTS[("*", HDR.format(size))] = (
        "Generic", f"{size} 2.54mm male header", f"{pins}-pin 2.54mm male pin header, vertical",
        "THT", "")

# The DevKitC carrier's two DevKitC footprints get sockets; the customer
# supplies and plugs in the DevKitCs.
PARTS[("*", DEVKITC)] = (
    "Generic", "1x22 2.54mm female header",
    "Socket for an ESP32-S3-DevKitC-1: two 1x22 2.54mm female headers per designator",
    "THT", "Fit 2 x 1x22 female headers per designator (4 total for U1+U2); DevKitC boards NOT supplied")

# Footprints that are not parts: the XE board's cartridge edge fingers.
SKIP = {"Atari8:CART+ECI_V3.0"}


def lookup(value, footprint):
    return PARTS.get((value, footprint)) or PARTS.get(("*", footprint))


def main():
    board, outdir = sys.argv[1], sys.argv[2]
    name = os.path.splitext(os.path.basename(board))[0]
    with tempfile.TemporaryDirectory() as tmp:
        pos = os.path.join(tmp, "pos.csv")
        subprocess.run([KICAD_CLI, "pcb", "export", "pos", "--format", "csv", "--units", "mm",
                        "--side", "both", "--exclude-dnp", "--output", pos, board],
                       check=True, stdout=subprocess.DEVNULL)
        with open(pos, newline="") as f:
            rows = list(csv.DictReader(f))

    groups, cpl, missing = {}, [], []
    for r in rows:
        ref, val, fpname = r["Ref"], r["Val"], r["Package"]
        # The pos file gives the footprint name without its library.
        fp = next((k[1] for k in PARTS if k[1].split(":")[1] == fpname), None)
        fp = fp or next((s for s in SKIP if s.split(":")[1] == fpname), fpname)
        if fp in SKIP:
            continue
        part = lookup(val, fp)
        if not part:
            missing.append(f"{ref}: {val!r} {fp!r}")
            continue
        groups.setdefault(part, []).append(ref)
        side = "Top" if r["Side"] == "top" and fp not in BOTTOM_INSERTED else "Bottom"
        cpl.append((ref, r["PosX"], r["PosY"], side, r["Rot"]))
    if missing:
        sys.exit("assembly_files.py: no PARTS entry for\n  " + "\n  ".join(missing))

    def refkey(ref):
        head = ref.rstrip("0123456789")
        return head, int(ref[len(head):] or 0)

    bom = os.path.join(outdir, f"{name}-bom.csv")
    with open(bom, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Item #", "Designator", "Qty", "Manufacturer", "Mfg Part #",
                    "Description / Value", "Package/Footprint", "Type", "Your Instructions / Notes"])
        items = sorted(groups.items(), key=lambda kv: refkey(min(kv[1], key=refkey)))
        for i, ((mfr, mpn, desc, pkg, notes), refs) in enumerate(items, 1):
            refs.sort(key=refkey)
            kind = "THT" if pkg == "THT" else "SMD"
            w.writerow([i, ",".join(refs), len(refs), mfr, mpn, desc, pkg, kind, notes])

    with open(os.path.join(outdir, f"{name}-cpl.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for ref, x, y, layer, rot in sorted(cpl, key=lambda c: refkey(c[0])):
            w.writerow([ref, f"{float(x):.4f}mm", f"{float(y):.4f}mm", layer, f"{float(rot):.1f}"])
    print(f"{name}: {len(cpl)} parts, {len(groups)} BOM lines")


if __name__ == "__main__":
    main()
