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
reports 103 items, all of kinds the DevKitC version has (fewer unconnected
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
| microSD | none | J2, a push-pull microSD socket (Hirose DM3AT-SF-PEJM5, `Connector_Card:microSD_HC_Hirose_DM3AT-SF-PEJM5`) in place of the DevKitC carrier's breakout header: same `SD_*` nets on IO1, IO2, IO42, IO41, IO40, IO39, R2-R6's pull-ups, VDD from `SC_3V3` with C10 100 n, shield to GND. Card detect: the socket's switch runs from GND (DET_B) to `SD_CD` (DET_A), pulled up by R14 10 k to `SC_3V3`, read on Sidecar IO14 |
| RGB status LED | none: GPIO48 is the link's BCLK | D1 SK6812MINI (WS2812 protocol) on GPIO48 (`SC_LED`), as on the DevKitC-1 v1.0, from `SC_3V3`, with C9 100 n; DOUT unconnected |

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

## Card detect

`SD_CD` (Sidecar IO14) follows the socket's detect switch, so the Sidecar can
see card swaps and remount or unmount instead of finding out on the next
failed read. Whether the switch closes or opens on insertion is in Hirose's
DM3AT datasheet; firmware is safest treating any edge (debounced) as "card
changed" and then trying to mount. IO14 is an input in both the main and the
Sidecar app, so a wrong image can't fight the switch; IO38, the other free
pin, is the main app's D0, which it drives.

## Soldering

Every resistor and capacitor uses KiCad's 0805 hand-solder footprints
(`R_0805_2012Metric_Pad1.20x1.40mm_HandSolder`,
`C_0805_2012Metric_Pad1.18x1.45mm_HandSolder`): longer pads with room for an
iron. The 10 µF and 22 µF caps are 0805 too. The others, easiest first:

| Part | Package | |
|---|---|---|
| U3, U4 AMS1117-3.3 | SOT-223 | big leads and tab: easy |
| U1, U2 WROOM-1 | castellated module | edge pads: doable with an iron; the centre GND pad needs hot air or a via-and-solder trick |
| J2 microSD socket | 1.1 mm pitch, shield tabs | fiddly: flux and a fine tip, or hot air |
| D1 SK6812MINI | 3.5 × 3.5 mm, pads underneath | the hardest by iron. The 5 × 5 mm SK6812 or WS2812B (`LED_SMD:LED_SK6812_PLCC4_5.0x5.0mm_P3.2mm`, same protocol) has pads you can see |

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

- **Flashing the main module through the Sidecar** is wired (EN on Sidecar
  IO11, BOOT on the link's IO9, the main UART0 on IO12/IO13 through R12/R13:
  see espBusBangSidecar's README) but needs the Sidecar's serial-flasher
  firmware. J5 is the way in until then, and afterwards when it's broken.
- **The main module's USB pins (IO19/IO20) are unconnected.** Pads there would
  give native USB (serial and JTAG) without an adapter, but the earlier plan
  to reclaim them for `refresh_` and reset may want them.
- **No level shifting**: as on the DevKitC carriers, the ESP32-S3 pins meet
  the Atari's 5 V bus directly.
- **Antenna keep-out**: the WROOM-1's PCB antenna needs the board edge and a
  copper-free area if Wi-Fi is wanted.
