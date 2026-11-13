# sidecar_pcb_xl: the Sidecar carrier with bare modules, PBI (speculative)

(Formerly `sidecar_smd`. Its XE twin, [sidecar_pcb_xe](../sidecar_pcb_xe/), is
the same circuit on the 130XE's cartridge + ECI edge.)

The [espBusBangSidecar](../espBusBangSidecar/) carrier with both
ESP32-S3-DevKitC-1 boards replaced by bare **ESP32-S3-WROOM-1** modules
(N8R2 main, N16R8 Sidecar),
plus what the DevKitC boards used to provide. A sketch of where a production
board could go, not a design to build yet.

**Status: autorouted, not reviewed.** Routed by
[`tools/autoroute.sh`](../tools/autoroute.sh) (Freerouting, every net
including GND, then GND pours on both layers with stitching vias): DRC shows
no clearance errors and nothing unconnected. Not yet checked by a human for
signal quality (the bus lines' lengths, the antennas' surroundings) or
ordered.

**J1 (PBI) orientation, as on espBusBang:** J1's footprint is on the front
copper layer but the connector is soldered in from the **back**, opposite the
modules, and the board installs with the modules facing away from the Atari
and the antennas up. Then J1's odd row (pin 1, GND, at the left) is on top,
matching the original carrier installed (its DevKitC's USB ports point to the
right as seen from the front of the Atari). Don't flip J1 to the back layer
in KiCad: that mirrors its holes.

Layout: the modules' antennas and the microSD slot point off the top edge;
J1 runs along the bottom; the main module's support parts (J5, J6, J3, U3)
are in a left wing and the Sidecar's (J4, J7, U4) in a right wing, beside J1;
the passives sit next to the module pins they serve. The regulators U3 and
U4 are on the back, for hand soldering. Past J7, the right end carries the
optional NTSC jack J8 and its ladder (R18-R21). Board 124.5 × 35.6 mm.

The WROOM footprint's 12 thermal vias (in each module's ground pad) are
widened on this board from KiCad's 0.2 mm drill to 0.3 mm, same 0.6 mm pads:
OSH Park's minimum drill is 0.254 mm. DRC flags U1/U2 as differing from the
library (lib_footprint_mismatch); don't "update footprint from library"
without widening them again.

DRC leftovers: the modules' silkscreen overhangs the top edge with the antennas; the
"10/02/25" text from the original board sits on J1's pads; four GND pads
reach the pour through one thermal spoke instead of two (starved_thermal);
J1's footprint library ("footprints") isn't in the library table.

Everything in the espBusBangSidecar README still applies: the main board's
nets (link, J3, R/W regroup) and the firmware changes they need.

## Files

| File | |
|---|---|
| `sidecar_pcb_xl.kicad_sch` | Generated: don't edit by hand |
| `sidecar_pcb_xl.kicad_pro`, `.kicad_pcb` | Board: placement by hand, routing by [`../tools/autoroute.sh`](../tools/autoroute.sh) |
| [`../tools/make_sidecar_xl_schematic.py`](../tools/make_sidecar_xl_schematic.py) | Builds it from the espBusBangSidecar schematic and its netlist |

```
kicad-cli sch export netlist -o /tmp/sc.net hardware/espBusBangSidecar/espBusBangSidecar.kicad_sch
python3 hardware/tools/make_sidecar_xl_schematic.py \
    hardware/espBusBangSidecar/espBusBangSidecar.kicad_sch /tmp/sc.net \
    /usr/share/kicad/symbols hardware/sidecar_pcb_xl/sidecar_pcb_xl.kicad_sch
```

The modules, regulators and capacitors come from KiCad's standard libraries
(`RF_Module`, `Regulator_Linear`, `Device`). Regenerate after any change to
the DevKitC carrier, so the two stay in step.

Checked with kicad-cli: every signal net is identical to the DevKitC
carrier's, GPIO for GPIO; the only differences are the additions below. ERC
reports 94 items, all of kinds the DevKitC version has (fewer unconnected
pins; more "library not in the configuration" items, one per added part).

## What replaced each DevKitC

| Per module | Main (U1) | Sidecar (U2) |
|---|---|---|
| Module | ESP32-S3-WROOM-1-N8R2 (8 MB flash, 2 MB quad PSRAM), `RF_Module:ESP32-S3-WROOM-1` | ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM), same footprint |
| 3.3 V regulator | U3 AP7361C-33E (SOT-223, 1 A) from the board rail `+5V` | U4, same |
| Capacitors | C1 10 µ in, C2 22 µ out, C3 100 n at the module | C5, C6, C7 |
| EN (reset) | R8 10 k to 3V3, C4 1 µ to GND: the RC power-on reset the DevKitC had | R10, C8 (`SC_EN`, also on J4) |
| IO0 (boot mode) | R9 10 k to 3V3: boots from flash (also J5 pin 6; the net is `LINK_TX`) | R11 (`SC_BOOT`, also on J4 pin 6) |
| 3V3 net | `3V3` | `SC_3V3` (also J2's SD card and R2-R6) |
| microSD | none | J2, a push-pull microSD socket (Hirose DM3AT-SF-PEJM5, `Connector_Card:microSD_HC_Hirose_DM3AT-SF-PEJM5`) in place of the DevKitC carrier's breakout header: same `SD_*` nets on IO1, IO2, IO42, IO41, IO40, IO39, R2-R6's pull-ups, VDD from `SC_3V3` with C10 100 n, shield to GND. Card detect: the socket's switch runs from GND (DET_B) to `SD_CD` (DET_A), pulled up by R14 10 k to `SC_3V3`, read on Sidecar IO14 |
| Indicator | D2, an LED on MPD_: see below | D1 (below) |
| RGB status LED | none: GPIO48 is the link's BCLK | D1 SK6812MINI (WS2812 protocol) on GPIO48 (`SC_LED`), as on the DevKitC-1 v1.0, from `SC_3V3`, with C9 100 n; DOUT unconnected |

Strapping kept to the minimum: EN's RC and IO0's pull-up. IO45 and IO46 have
internal pull-downs that do the right thing, and IO3 only matters with JTAG
eFuses set. On the main module IO45 is D7 and IO46 is MPD (pulled up by the
Atari); see the boot-pin notes in `AGENTS.md` (burn `VDD_SPI_FORCE`; ROM
download mode needs the Atari off).

The AP7361C-33E (Diodes Inc.) is stable with ceramic output caps, so the
0805 ceramics work; the familiar AMS1117 needs a tantalum's ESR on its output
and can oscillate with ceramics. It drops out at ~0.36 V and, at a few hundred
mA of Wi-Fi current from 5 V, dissipates ~0.5 W: comfortable in SOT-223 with
some copper on the tab.

The bare module has no GPIOs the DevKitC lacks: the DevKitC's headers carry
all of them (GPIO26-32 are the module's own flash/PSRAM on either). What the
bare module gains is cleaner pins: on the DevKitC, GPIO43/44 (UART0) go to
the CP2102 bridge, whose TX drives GPIO44 (main D6); GPIO38 or GPIO48 drives
the RGB LED; and GPIO19/20 go to the USB port. Here those carry only what
the carrier connects.

## Main module: RFSH and RESET on the USB pins

(The DevKitC carrier can run this same wiring with its jumpers J8/J9 fitted;
see its README.)

With no USB port on the main module, its USB pins carry two Atari signals
the DevKitC carriers had to give up:

| Main pin | Net | |
|---|---|---|
| GPIO20 (USB D+) | `REFRESH` (PBI RFSH) | `refresh_` as its own input, so Sidecar builds keep it too |
| GPIO19 (USB D−) | `RESET_DRV` → R1 → `RESET` (PBI RESET) | as on the original espBusBang board, where GPIO48 had it |

At every reset the ROM enables the USB-Serial-JTAG PHY with a 1.5 k pull-up
on D+ until the app takes the pins. That's on RFSH, which ANTIC drives
actively, so it can't upset anything; RESET is on D−, which has no boot-time
pull-up. (Burning `DIS_USB_SERIAL_JTAG` would remove even that, at the cost
of USB download mode, which this module can't use anyway.)

J3 stays: with `refresh_` on GPIO20, GPIO21 no longer has to share, so a
later revision could drop J3 and wire GPIO21 to the link for good.

Firmware: `pinDefs.h` needs a board type for this layout (`refresh_` on
GPIO20 in every build, so `HAVE_REFRESH` holds with the Sidecar too; a reset
pin on GPIO19, open drain through R1), and the console is J5 or the link,
not USB.

## D2: the main module's indicator, on MPD_

The main module has no pin to spare for an LED, so D2 watches MPD_ instead:
3V3 → R17 (1 k) → D2 → `MPD` (main GPIO46, PBI pin 43). MPD_ is open drain
with the Atari's pull-up: the main module only ever pulls it low, so an LED
on it can never cause a pin-drive conflict.

- **Health:** core 1 asserts MPD_ while the 6502 runs the PBI ROM at
  `$D800`-`$DFFF`, so D2 flickers with PBI requests: a direct sign that the
  main board is serving the Atari.
- **Faults:** with the bus cold (no clock, a fatal exit, the startup timeout,
  a panic), the firmware can blink D2 in a pattern. Only then: asserting
  MPD_ while the Atari runs hides its floating-point ROM and crashes it.
  Core 1 drives MPD_ through a dedicated-GPIO channel; once it has stopped,
  core 0 can route GPIO46 to a plain open-drain GPIO and blink it.
- Released, the Atari's 5 V pull-up reverse-biases D2: dark, no current.
  With the Atari unpowered it can glow faintly (~30 µA through GPIO46's
  internal pull-down), which also keeps GPIO46 low, as download mode wants.

## J7: Sidecar breakout

Every Sidecar GPIO with no other job, except IO35-37, with power, on a 2×7 header
(`PinHeader_2x07_P2.54mm_Vertical`, odd pins in one row, even in the other).
3V3 and GND are the end pair; USB D−/D+ sit right beside them; a second GND
fills the far end.

| Pin | Net | Pin | Net |
|---|---|---|---|
| 1 | 3V3 (`SC_3V3`) | 2 | GND |
| 3 | IO19 (USB D−) | 4 | IO20 (USB D+) |
| 5 | IO3 (strapping: JTAG source; leave floating at reset) | 6 | IO8 |
| 7 | IO18 | 8 | IO21 |
| 9 | NC (IO35: octal PSRAM) | 10 | NC (IO36) |
| 11 | NC (IO37) | 12 | IO38 |
| 13 | IO45 (strapping: VDD_SPI; low at reset unless the eFuse is burned) | 14 | GND |

Pins 2-4 are the Sidecar's native USB (GND, D−, D+): a USB cable breakout
there gives USB serial/JTAG without an adapter. The Sidecar's N16R8 uses
IO35-37 inside the module for its octal PSRAM, so pins 9-11 are left
unconnected; they keep their places so an N8R2 Sidecar could have them back
without moving the other pins. The main module is an N8R2 (quad PSRAM), so
its IO35-37 stay free. The Sidecar firmware must be built for octal PSRAM
(`CONFIG_SPIRAM_MODE_OCT`) and 16 MB flash to use the extra memory.

J7 pins 6, 7, 8 and 12 (IO8, IO18, IO21, IO38) are the ones the
composite-video ladder uses (J8, below): with R18-R21 fitted they're loaded
by the ladder and the TV.

## J8: NTSC composite video (optional)

A grayscale NTSC output, all five parts DNP (fit all or none): a 4-bit
resistor ladder from the Sidecar into an RCA jack on the board's right edge,
plug entering from the side.

| Ref | Value | From | To |
|---|---|---|---|
| R18 | 2.7 k (bit 0) | IO8 (`SC_IO8`) | `NTSC_VIDEO` |
| R19 | 1.3 k (bit 1) | IO18 | `NTSC_VIDEO` |
| R20 | 680 Ω (bit 2) | IO21 | `NTSC_VIDEO` |
| R21 | 330 Ω (bit 3) | IO38 | `NTSC_VIDEO` |
| J8 | Same Sky RCJ-041 (Digi-Key CP-1418-ND) | centre: `NTSC_VIDEO`, shell: GND | |

No resistor to ground: the TV's 75 Ω input is the load. Levels, the
firmware approach (LCD_CAM) and drive settings are in
[espBusBangSidecar's README](../espBusBangSidecar/README.md#composite-video-experimental-off-board).
The resistors are 0805 hand-solder like the rest; the jack is through
hole. Any RCJ-04x fits (the last digit is the housing colour).

J8's footprint, `Sidecar:RCA_SameSky_RCJ-04x_Horizontal` in
[`../Sidecar.pretty`](../Sidecar.pretty) (the projects' `fp-lib-table`
points KiCad at it), is from the datasheet's recommended layout
(rev 1.07): plated slots for the two terminals and board cutouts for the
two front locating feet, with the body's front face 1.5 mm inside the
edge so the cutouts stay closed. Its pads follow KiCad's `Conn_Coaxial`
symbol, the reverse of the datasheet's numbering: pad 1 is the centre
contact (datasheet terminal 2, 8.5 mm back), pad 2 the shell (terminal 1,
4 mm back, the nickel-plated lug under the barrel).

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
| U3, U4 AP7361C-33E | SOT-223 | big leads and tab: easy |
| U1, U2 WROOM-1 | castellated module | edge pads: doable with an iron; the centre GND pad needs hot air or a via-and-solder trick |
| J2 microSD socket | 1.1 mm pitch, shield tabs | fiddly: flux and a fine tip, or hot air |
| D2 indicator LED | 0805, hand-solder pads | easy; mind the polarity mark |
| D1 SK6812MINI | 3.5 × 3.5 mm, pads underneath | the hardest by iron. The 5 × 5 mm SK6812 or WS2812B (`LED_SMD:LED_SK6812_PLCC4_5.0x5.0mm_P3.2mm`, same protocol) has pads you can see |

## J6: board power

| Jumper | The modules run from |
|---|---|
| On | the Atari: PBI 5 V (`VCC50_1`, J1.47) feeds the board rail (`+5V`). Only the 600XL puts 5 V on that PBI pin; most Ataris don't |
| Off | J4 or J5's VDD pin (an ESP-Prog set to 5 V, or a bench supply); the Atari's 5 V rail is never back-fed |

Both regulators and both debug headers are on `+5V`; only J6 connects it to
the PBI connector.

## J4 and J5: programming headers (ESP-Prog)

Both follow Espressif's [ESP-Prog](https://docs.espressif.com/projects/esp-dev-kits/en/latest/other/esp-prog/user_guide.html)
"Program interface", 2×3 at 2.54 mm (`PinHeader_2x03_P2.54mm_Vertical`), so
an ESP-Prog plugs straight in and `idf.py flash` / esptool work with no
buttons: the ESP-Prog does the DTR/RTS auto-reset itself and drives EN and
IO0 directly, so the board needs no extra parts. J4 is the Sidecar's, J5 the
main module's.

| Pin | ESP-Prog | J4 (Sidecar) | J5 (main) |
|---|---|---|---|
| 1 | ESP_EN | EN (`SC_EN`) | EN (`MAIN_EN`) |
| 2 | VDD | +5V rail | +5V rail |
| 3 | ESP_TXD | TXD0, GPIO43 (`SC_TXD`) | TXD0, GPIO43 (`D5`) |
| 4 | GND | GND | GND |
| 5 | ESP_RXD | RXD0, GPIO44 (`SC_RXD`) | RXD0, GPIO44 (`D6`) |
| 6 | ESP_IO0 | IO0 (`SC_BOOT`) | IO0 (`LINK_TX`) |

ESP_TXD/ESP_RXD are named from the target's side (the ESP-Prog's own RX
reads ESP_TXD); the guide implies it rather than saying it, so check one
connection the first time.

**Power.** Pin 2 feeds the board's 5 V rail, ahead of the AMS1117s, so set
the ESP-Prog's PROG PWR SEL to 5 V (at 3.3 V the regulators can't start the
modules). With J6 fitted and the Atari on, the rail already has 5 V: don't
also power it from the ESP-Prog (check the ESP-Prog schematic for how to
leave VDD unconnected, or leave J6 open).

**J5 is for use with the Atari unplugged.** Its TXD/RXD are the Atari's D5/D6
(reached through R15/R16, 1 k, which limit what an adapter can push into the
unpowered Atari's chips), and download mode needs IO46 (MPD, pulled up by a
powered Atari) low anyway. Unplug the ESP-Prog before plugging the Atari back
in: its TX idles high.

The ESP-Prog also has the same interface at 1.27 mm, with a cable to a
shrouded 2×3 box header; that footprint would fit too if a keyed connector is
wanted. A generic USB-serial adapter can still be wired to these pins with
dupont leads, with a BOOT jumper (pin 6 to GND) instead of auto-reset.

## Power during development

Most Ataris don't put 5 V on the PBI connector (only the 600XL does), so for
now the board runs from J5's (or J4's) VDD pin: an ESP-Prog set to 5 V, or a
bench supply, with J6 open.

## Flashing rules

- **Serial flashing of the main module** (J5, or the Sidecar's serial
  flasher): only with the Atari unplugged.
- **The Sidecar** (J4): any time; its UART pins touch nothing on the Atari.
- **The unattended test-farm machines** flash the main module only by OTA,
  over the link from the Sidecar, never serially.
- **First flash of a new main module**, before it ever sees the Atari: burn
  `VDD_SPI_FORCE` (IO45 is D7; `tools/efuse_check.sh` does it, and `boot.sh`
  runs it, through J5 as through USB).

## Open questions before this could be built

- **Flashing the main module through the Sidecar** is wired (EN on Sidecar
  IO11, BOOT on the link's IO9, the main UART0 on IO12/IO13 through R12/R13:
  see espBusBangSidecar's README) but needs the Sidecar's serial-flasher
  firmware. J5 is the way in until then, and afterwards when it's broken.
- **The main module has no USB**: its USB pins carry RFSH and RESET, so its
  console and flashing are J5 or the Sidecar (link, serial flasher).
- **No level shifting**: as on the DevKitC carriers, the ESP32-S3 pins meet
  the Atari's 5 V bus directly.
- **Antenna keep-out**: the WROOM-1's PCB antenna needs the board edge and a
  copper-free area if Wi-Fi is wanted.
