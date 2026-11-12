# sidecar_smd: the Sidecar carrier with bare modules (speculative)

The [espBusBangSidecar](../espBusBangSidecar/) carrier with both
ESP32-S3-DevKitC-1 boards replaced by bare **ESP32-S3-WROOM-1-N8R2** modules,
plus what the DevKitC boards used to provide. A sketch of where a production
board could go, not a design to build yet.

**Status: schematic only.** `sidecar_smd.kicad_pcb` is an unchanged copy of
the DevKitC carrier's board (it still has the DevKitC footprints); run Update
PCB from Schematic (F8) and lay it out from scratch.

Everything in the espBusBangSidecar README still applies: the main board's
nets (link, J3, R/W regroup) and the firmware changes they need.

## Files

| File | |
|---|---|
| `sidecar_smd.kicad_sch` | Generated: don't edit by hand |
| `sidecar_smd.kicad_pro`, `.kicad_pcb` | Copied from espBusBangSidecar (filenames updated) |
| [`../tools/make_sidecar_smd_schematic.py`](../tools/make_sidecar_smd_schematic.py) | Builds it from the espBusBangSidecar schematic and its netlist |

```
kicad-cli sch export netlist -o /tmp/sc.net hardware/espBusBangSidecar/espBusBangSidecar.kicad_sch
python3 hardware/tools/make_sidecar_smd_schematic.py \
    hardware/espBusBangSidecar/espBusBangSidecar.kicad_sch /tmp/sc.net \
    <kicad>/share/kicad/symbols hardware/sidecar_smd/sidecar_smd.kicad_sch
```

The modules, regulators and capacitors come from KiCad's standard libraries
(`RF_Module`, `Regulator_Linear`, `Device`). Regenerate after any change to
the DevKitC carrier, so the two stay in step.

Checked with kicad-cli: every signal net is identical to the DevKitC
carrier's, GPIO for GPIO; the only differences are the additions below. ERC
reports 102 items, all of kinds the DevKitC version has (fewer unconnected
pins; more "library not in the configuration" items, one per added part).

## What replaced each DevKitC

| Per module | Main (U1) | Sidecar (U2) |
|---|---|---|
| Module | ESP32-S3-WROOM-1-N8R2, `RF_Module:ESP32-S3-WROOM-1` | same |
| 3.3 V regulator | U3 AMS1117-3.3 (SOT-223) from the board rail `+5V` | U4, same |
| Capacitors | C1 10 µ in, C2 22 µ out, C3 100 n at the module | C5, C6, C7 |
| EN (reset) | R8 10 k to 3V3, C4 1 µ to GND: the RC power-on reset the DevKitC had | R10, C8 (`SC_EN`, also on J4) |
| IO0 (boot mode) | R9 10 k to 3V3: boots from flash (also J5; the net is `LINK_TX`) | R11 (`SC_BOOT`, also on J4) |
| 3V3 net | `3V3` | `SC_3V3` (also J2's SD card and R2-R6) |

Strapping kept to the minimum: EN's RC and IO0's pull-up. IO45 and IO46 have
internal pull-downs that do the right thing, and IO3 only matters with JTAG
eFuses set. On the main module IO45 is D7 and IO46 is MPD (pulled up by the
Atari); see the boot-pin notes in `AGENTS.md` (burn `VDD_SPI_FORCE`; ROM
download mode needs the Atari off).

The AMS1117 drops out at ~1.1 V, so 5 V in is fine; at a few hundred mA of
Wi-Fi current it dissipates ~0.5 W, comfortable in SOT-223 with some copper.

The bare module has no GPIOs the DevKitC lacks: the DevKitC's headers carry
all of them (GPIO26-32 are the module's own flash/PSRAM on either). What the
bare module gains is cleaner pins: on the DevKitC, GPIO43/44 (UART0) go to
the CP2102 bridge, whose TX drives GPIO44 (main D6); GPIO38 or GPIO48 drives
the RGB LED; and GPIO19/20 go to the USB port. Here those carry only what
the carrier connects.

## J6: board power

| Jumper | The modules run from |
|---|---|
| On | the Atari: PBI 5 V (`VCC50_1`, J1.47) feeds the board rail (`+5V`) |
| Off | J4 or J5's +5V pin (a USB-serial adapter or bench supply); the Atari's 5 V rail is never back-fed |

Both regulators and both debug headers are on `+5V`; only J6 connects it to
the PBI connector.

## J4 and J5: debug/flash headers

The same pinout on both, so one adapter fits either. J4 is the Sidecar's, J5
the main module's.

| Pin | J4 (Sidecar) | J5 (main) | |
|---|---|---|---|
| 1 | +5V | +5V | the board rail: powers both modules with J6 off |
| 2 | GND | GND | |
| 3 | BOOT: IO0 (`SC_BOOT`) | BOOT: IO0 (`LINK_TX`) | hold low through reset for ROM download mode |
| 4 | EN (`SC_EN`) | EN (`EN`) | low = reset |
| 5 | RX: RXD0, GPIO44 (`SC_RXD`) | RX: RXD0, GPIO44 (`D6`) | the adapter's TX |
| 6 | TX: TXD0, GPIO43 (`SC_TXD`) | TX: TXD0, GPIO43 (`D5`) | the adapter's RX |

BOOT and EN take an adapter's DTR/RTS through the usual two-transistor
auto-reset circuit, or a hand-held jumper.

**J5 is for use with the Atari off (J6 open, Atari unpowered).** Its RX/TX are
the Atari's D6/D5, and download mode needs IO46 (MPD, pulled up by a powered
Atari) low anyway. Normal flashing of the main module is meant to go through
the Sidecar over the link; J5 is the way in when that isn't working.

## Open questions before this could be built

- **Flashing the main module through the Sidecar** (OTA over the link) needs
  firmware on both sides, and probably a Sidecar pin on the main module's EN
  so the Sidecar can reset it. J5 covers it until then.
- **The main module's USB pins (IO19/IO20) are unconnected.** Pads there would
  give native USB (serial and JTAG) without an adapter, but the earlier plan
  to reclaim them for `refresh_` and reset may want them.
- **No level shifting**: as on the DevKitC carriers, the ESP32-S3 pins meet
  the Atari's 5 V bus directly.
- **Antenna keep-out**: the WROOM-1's PCB antenna needs the board edge and a
  copper-free area if Wi-Fi is wanted.
