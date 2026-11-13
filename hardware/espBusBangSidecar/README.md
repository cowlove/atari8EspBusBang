# espBusBangSidecar: the carrier with a Sidecar

A next-generation version of the [espBusBang](../espBusBang/) carrier with a
second ESP32-S3-DevKitC-1, the **Sidecar**, beside the main board. The
Sidecar talks to the main board over the I2S link and can run disk I/O, a bus
profiler, a microSD card and an audio output.

**Status: autorouted, not reviewed.** Placed by
[`../tools/place_sidecar_devkitc.py`](../tools/place_sidecar_devkitc.py) and
routed by [`../tools/autoroute.sh`](../tools/autoroute.sh) (see
[../AGENTS.md](../AGENTS.md)): no clearance errors, nothing unconnected,
schematic parity clean. Not checked by a human or ordered yet.

**Mounting, as on espBusBang:** J1 (PBI) is soldered in from the **back**,
the DevKitC sockets on the front, so the DevKitCs face out from the Atari:
there is no room for them on J1's side. (Both footprints sit on the front
layer in KiCad, as on espBusBang: don't flip J1.) U1 and J1 are exactly where
espBusBang has them; U2 sits beside U1, on the side away from J1's mounting
edge, and the headers (J2–J5, J7–J10) and the MPD_ LED (D2) run in a strip
beyond U2, outside the sockets. The small SMD resistors sit between the
DevKitCs' header rows, as R1 does on espBusBang: solder them before the
sockets. Board 71.3 × 63.8 mm; the right edge moved out 0.26 mm so U1's
outer pads clear the 0.5 mm edge clearance (espBusBang has 0.38 mm).

DRC leftovers: J1's pins 49/50 sit 0.38 mm from the top edge (as on
espBusBang); the DevKitC courtyards overlap the parts under them and J1
(intended); silkscreen overlaps from the DevKitC footprint's pin labels.

**The main board's firmware must change before this board can be used**: see
[Software changes required](#software-changes-required). The R/W pin moved.

## Files

| File | |
|---|---|
| `espBusBangSidecar.kicad_sch` | Generated: don't edit by hand |
| `espBusBangSidecar.kicad_pro`, `.kicad_pcb` | Board: from espBusBang's, updated by [`../tools/netlist_to_pcb.py`](../tools/netlist_to_pcb.py), placed and routed as above |
| [`../tools/make_sidecar_schematic.py`](../tools/make_sidecar_schematic.py) | Builds the schematic from `../espBusBang/atariEspBusBang3.kicad_sch` |
| [`../../docs/sidecar_wiring.pdf`](../../docs/sidecar_wiring.pdf) | The board-to-board wiring as a drawing (from [`../tools/sidecar_wiring.py`](../tools/sidecar_wiring.py)) |

To change the schematic, edit the script and regenerate:

```
python3 hardware/tools/make_sidecar_schematic.py \
    hardware/espBusBang/atariEspBusBang3.kicad_sch \
    hardware/espBusBangSidecar/espBusBangSidecar.kicad_sch
kicad-cli sch erc --severity-all -o erc.rpt hardware/espBusBangSidecar/espBusBangSidecar.kicad_sch
kicad-cli sch export netlist -o sidecar.net hardware/espBusBangSidecar/espBusBangSidecar.kicad_sch
```

ERC reports 83 items. None are new kinds of problem: the same classes the
original board has (labels on one-pin nets, no
PWR_FLAG, J1 pins of type "Unspecified"), plus "library not in the
configuration" when KiCad's library tables lack Espressif and the TE
connector.

## Parts added

Every resistor and LED, R1 from espBusBang included, uses KiCad's 0805
hand-solder footprints (longer pads, for an iron); only headers and
connectors are through-hole.

| Ref | Part | Footprint |
|---|---|---|
| U2 | ESP32-S3-DevKitC-1 (the Sidecar) | `Espressif:ESP32-S2-DevKitC-1`, as U1 |
| J2 | 1×8 header for a microSD breakout, in the card's pin order: DAT2, DAT3, CMD, 3V3, CLK, GND, DAT0, DAT1. An SDIO breakout (pins D0–D3, CMD, CLK) wires 1:1 and runs 4-bit. An SPI-only breakout works too, at about a quarter of the speed: CS → 2, DI/MOSI → 3, CLK → 5, DO/MISO → 7, GND → 6; if it has its own regulator, feed it 5 V (`SC_5V`) rather than pin 4's 3.3 V. The Sidecar tries SD mode, then SPI mode | `PinHeader_1x08_P2.54mm_Vertical` |
| R2–R6 | 10 k pull-ups to the Sidecar's 3V3 on SD CMD and DAT0–DAT3 | `R_0805_2012Metric_Pad1.20x1.40mm_HandSolder` |
| R7 | 1 k series resistor, Sidecar audio to the PBI audio input; sets the level, a starting guess | 0805 hand-solder |
| R12, R13 | 1 k series resistors in the Sidecar's flashing UART, to the main board's UART0 (Atari D6/D5) | 0805 hand-solder |
| J3 | 1×3 header: main GPIO21 selector | `PinHeader_1x03_P2.54mm_Vertical` |
| J8, J9, J10 | 2-pin jumpers: RFSH and RESET to the main USB pins, Sidecar 5V to the carrier's 5V | `PinHeader_1x02_P2.54mm_Vertical` |
| J4 | Sidecar ESP-Prog header (2×3): 1 EN (`SC_EN`), 2 VDD (`SC_5V`), 3 TXD0 (GPIO43), 4 GND, 5 RXD0 (GPIO44), 6 IO0 (`SC_BOOT`), as on sidecar_pcb_xl. The DevKitC's own USB-UART is on the same TXD0/RXD0: use one or the other | `PinHeader_2x03_P2.54mm_Vertical` |
| J5, R15, R16 | Main ESP-Prog header: 1 EN (`MAIN_EN`), 2 VDD (`VCC50_1`), 3/5 TXD0/RXD0 through R15/R16 (1 k) to Atari D5/D6, 6 IO0 (`LINK_TX`). **Atari unplugged** (its UART is the data bus); an ESP-Prog at 5 V back-feeds the PBI 5 V, as the DevKitC's USB does | 2×3 header, 0805 R |
| J7 | Sidecar GPIO breakout (2×8): 1 `SC_3V3`, 2 GND, 3–15 the unused Sidecar GPIOs (19, 20, 3, 8, 14, 18, 21, 35, 36, 37, 38, 45, 48), 16 GND. On the DevKitC, GPIO19/20 are its USB port and GPIO48 its RGB LED; GPIO3 and 45 are straps | `PinHeader_2x08_P2.54mm_Vertical` |
| D2, R17 | MPD_ activity LED: main 3V3 → R17 (1 k) → D2 → `MPD`, lit while MPD_ is asserted | `LED_0805_2012Metric_Pad1.15x1.40mm_HandSolder`, 0805 R |

## J3: main board GPIO21

| Jumper | Main GPIO21 is | Use with |
|---|---|---|
| 1–2 | the PBI RFSH line (`refresh_`), as on espBusBang | firmware built without the Sidecar (`CONFIG_A8BB_SIDECAR_ENABLE` off), Sidecar unused or running stand-alone |
| 2–3 | the link's RX, from Sidecar GPIO46 | firmware built with `CONFIG_A8BB_SIDECAR_ENABLE` |

The Sidecar's own RFSH input (GPIO17) is wired to RFSH directly, whichever
way J3 is set.

## J8–J10: standing in for sidecar_pcb_xl

Three 2-pin jumpers let this board run the [sidecar_pcb_xl](../sidecar_pcb_xl/)
wiring, where the bare main module has no USB port and its USB pins carry
two Atari signals:

| Jumper | Joins | Fitted |
|---|---|---|
| J8 | PBI RFSH (`REFRESH`) and main GPIO20, USB D+ (`U1_GPIO20`) | `refresh_` on its own pin, in every build |
| J9 | R1's ESP end (`RESET_DRV`, R1 to PBI RESET) and main GPIO19, USB D− (`U1_GPIO19`) | the Atari's reset under the main board's control |
| J10 | the carrier's 5 V (`VCC50_1`: PBI 5 V, the main DevKitC's 5V pin) and the Sidecar's 5V pin (`SC_5V`) | one 5 V for both boards |

- **J8 and J9 open** (the default): the main DevKitC's USB port works as it
  does today, and it's the console.
- **J8 and J9 fitted**: the main board's USB lines carry RFSH and RESET, so
  its USB port is unusable (don't plug it in); its console is the link.
  Firmware must be built for the sidecar_pcb_xl pin layout (below).
- **J10 open**: the Sidecar runs from its own USB, and nothing it's plugged
  into reaches the Atari's 5 V. **Fitted**: the Atari powers it too.

With J3 on 2–3 and J8–J10 fitted, this board is wired as sidecar_pcb_xl, except
for the DevKitC's own extras (its USB-serial bridge on main D5/D6, its LED).

## D2: MPD_ activity LED

The same indicator as sidecar_pcb_xl's (its README has the details): D2 lights
whenever MPD_ is asserted, which core 1 does while the 6502 runs the PBI ROM,
so it flickers with PBI requests. The main board only ever pulls MPD_ low, so
the LED can't cause a drive conflict. Fault blinking (only with the bus cold)
can be developed here for the bare-module board.

## Net changes on the main board (U1) vs espBusBang

| U1 pin | espBusBang | This board | Why |
|---|---|---|---|
| GPIO0 | `AUDIO` (PBI 49) | `LINK_TX` → Sidecar GPIO9 | The link's TX loaded PBI audio and could keep the link from locking (miner6 board 5). PBI audio is now driven by the Sidecar. |
| GPIO21 | `REFRESH` (PBI RFSH) | J3 pin 2 | Shared between `refresh_` and the link's RX; J3 picks. |
| GPIO48 | R1 → `RESET` | `LINK_BCLK` → Sidecar GPIO10 | BCLK would toggle the Atari's reset through R1. R1's ESP end (`RESET_DRV`) moves to J9. |
| GPIO19 (USB D−) | USB port only | also J9 → `RESET_DRV` → R1 → `RESET` | the sidecar_pcb_xl layout, jumper-selectable |
| GPIO20 (USB D+) | USB port only | also J8 → `REFRESH` | the sidecar_pcb_xl layout, jumper-selectable |
| GPIO15 | `A14` | `READ` (R/W) | **R/W regroup** (below) |
| GPIO16 | `A15` | `A14` | R/W regroup |
| GPIO17 | `READ` (R/W) | `A15` | R/W regroup |

### The R/W regroup

`r0` (GPIO_IN_REG) used to carry A0–A15 contiguously at bits 1–16 and R/W at
bit 17. Core 1's ctrl[] index is the 6 page bits A8–A13 with R/W as bit 6,
so it took an `srli`/`and`/`or` to splice R/W in. With R/W at bit 15, right
above A13, one `extui r0, 9, 7` gives the index:

```
today                                  regrouped
extui  a4, r0, 15, 2   ; block         extui  a4, r0, 16, 2   ; block
srli   a10, r0, 11     ; R/W           addx4  a4, a4, base
addx4  a4, a4, base                    l32i   a4, a4, 4
extui  a3, r0, 9, 6    ; page          extui  a3, r0, 9, 7    ; page + R/W
l32i   a4, a4, 4                       addx2  a3, a3, a4
and    a10, a10, 64                    l16ui  a3, a3, 0
or     a10, a3, a10                    ee.wr_mask_gpio_out a3, a3
addx2  a3, a10, a4
l16ui  a3, a3, 0
ee.wr_mask_gpio_out a3, a3
```

Three cycles (~12.5 ns) off the address-read-to-extSel_ path (P1 to P2: 15 to
12 static cycles) and off every loop. The cost: the address is no longer a
contiguous bit field in `r0`.

## The Sidecar (U2)

| U2 pin | Net | Use |
|---|---|---|
| GPIO9 | `LINK_TX` | link RX, from main GPIO0 |
| GPIO46 | `LINK_RX` | link TX, to main GPIO21 via J3 2–3 |
| GPIO10 | `LINK_BCLK` | link clock, from main GPIO48 |
| GPIO4, 5, 6, 7, 15, 16 | `PHI2`, `READ`, `EXTSEL`, `D0`, `D1`, `A0` | bus profiler inputs |
| GPIO17 | `REFRESH` | bus profiler `refresh_` (PBI RFSH) |
| GPIO1, 2, 42, 41, 40, 39 | `SD_DAT2`, `SD_DAT3`, `SD_CMD`, `SD_CLK`, `SD_DAT0`, `SD_DAT1` | microSD, 4-bit SDMMC (J2) |
| GPIO47 | `SC_AUDIO` | audio out (PWM or sigma-delta) → R7 → PBI `AUDIO` |
| GPIO11 | `MAIN_EN` | the main board's EN (CHIP_PU): open drain, low = reset |
| GPIO12 | `SC_FLASH_TX` | → R12 → `D6` = the main board's RXD0 (GPIO44) |
| GPIO13 | `SC_FLASH_RX` | ← R13 ← `D5` = the main board's TXD0 (GPIO43) |
| 5V | `VCC50_1` | powered like U1 |
| 3V3 | `SC_3V3` | its own 3V3, for J2 and R2–R6 only; never tied to U1's 3V3 |

The pins follow the rules in `sidecar/main/sidecarApp.cpp`: no strapping, USB,
UART0, flash/octal-PSRAM or RGB-LED pins, and a wrong image on either board
can't make two outputs fight (GPIO47 meets the main app's open-drain extSel_;
the rest are main-app inputs).

## Composite video (experimental, off-board)

Grayscale NTSC composite from the Sidecar, for development: a 4-bit
resistor ladder from four J7 pins into the TV's 75 Ω input. The S3 has no
DAC; the Sidecar app would stream 4-bit samples to the pins with the
LCD_CAM peripheral (8-bit i8080 mode, DMA, exact pixel clock), leaving I2S
to the link. Nothing for it is on this board: build the ladder on a small
adapter on J7. sidecar_pcb_xl and sidecar_pcb_xe carry it as optional
(DNP) parts: R18-R21 and an RCA jack, J8 (Same Sky RCJ-041), on the
Sidecar's end of the board (see their READMEs). The pins are the four with no strapping, USB, LED or PSRAM
role on sidecar_pcb_xl's N16R8 Sidecar, so the same wiring carries over.

| Bit | Sidecar GPIO | J7 pin (here) | J7 pin (sidecar_pcb_xl/xe) | Resistor to the output node |
|---|---|---|---|---|
| 0 (LSB) | IO8 | 6 | 6 | 2.7 k |
| 1 | IO18 | 8 | 7 | 1.3 k |
| 2 | IO21 | 9 | 8 | 680 Ω |
| 3 (MSB) | IO38 | 13 | 12 | 330 Ω |
| ground | | 2 or 16 | 2 or 14 | RCA shell |

The four resistors' far ends join at the RCA centre pin. No resistor to
ground: the TV's 75 Ω input is the load. E24 values, 1 % or 5 %: the pins'
own output resistance (about 30 Ω, a guess) matters more than the
tolerance. Set the four pins to the strongest drive
(`GPIO_DRIVE_CAP_3`) so that resistance is small.

Levels into 75 Ω (3.3 V outputs; the first figure ignores the pins'
resistance, the second assumes 30 Ω per pin):

| Code | Level | Use | NTSC target |
|---|---|---|---|
| 0 | 0 V | sync tip | 0 V (−40 IRE) |
| 4 | 0.256 / 0.249 V | blanking (and the colour-burst level) | 0.286 V (0 IRE) |
| 5 | 0.320 / 0.314 V | black | 0.339 V (7.5 IRE setup) |
| 6–14 | 0.39 to 0.92 V | 9 grays | |
| 15 | 0.981 / 0.939 V | white | 1.0 V (100 IRE) |

That gives 11 shades from black to white. The steps run 64–70 mV,
except code 7 to 8 (the MSB switching in), which may come out 45–70 mV
depending on the pins' resistance. If white measures low, drop the MSB
resistor to 300 Ω. Measure with a 75 Ω terminator on the output: unloaded,
the node swings much higher (up to 3.3 V). The ladder's output impedance
is about 180 Ω instead of 75 Ω; that's fine for a short cable to a TV.
At white the MSB pin sources about 7 mA, the others less.

On an ESP32-S3-DevKitC-1 v1.1 the RGB LED is on GPIO38 (on v1.0 it's
GPIO48): the LED then shows noise while video runs, harmlessly. If that
matters, move bit 3 to IO14 (J7 pin 7), free on the DevKitC carrier but
the card-detect line on sidecar_pcb_xl.

## Serial flashing of the main board by the Sidecar

The Sidecar has everything esptool's serial protocol needs to flash the main
board, as a USB-serial adapter with DTR/RTS would:

| Main board | From the Sidecar |
|---|---|
| EN | GPIO11 (`MAIN_EN`), open drain: pull low to reset, release to run |
| IO0 (BOOT) | GPIO9 (`LINK_TX`, the link's RX pin): drive it low while releasing EN to enter ROM download mode, then return it to the link |
| RXD0 (GPIO44) | GPIO12, UART TX, via R12 |
| TXD0 (GPIO43) | GPIO13, UART RX, via R13 |

**Only with the Atari unplugged.** The unattended test-farm machines flash
the main board by OTA over the link only, never serially. The main board's
UART0 pins are Atari D6 and D5, and download mode needs IO46 (MPD, pulled up by a powered Atari) low.
The Sidecar keeps GPIO12/13 as inputs except while flashing; R12/R13 keep its
stubs off the data bus and limit the current if it ever drove TX with the
Atari on. With the Atari on, the Sidecar can still reset the main board
through `MAIN_EN` (a hung board, or OTA with rollback over the link).

On a DevKitC main board, its own CP2102 USB-serial bridge also drives GPIO44
when its UART USB port is powered: leave that port unplugged while the Sidecar
flashes.

**Firmware** (`sidecar/main/mainFlasher.cpp`): the Sidecar speaks the
ESP32-S3 ROM loader protocol (what esptool uses) over UART1. Stage the main
firmware in the Sidecar's SPIFFS, then type `flash` on the Sidecar's USB
console:

```
tools/flash_spiffs.sh --with-main -p $SIDECAR_PORT   # main build's 4 images -> /spiffs/main/
```

| Console command | |
|---|---|
| `flash` | bootloader, partition table, OTA data and app, each verified against the ROM's MD5, then a normal boot |
| `flash app` | just the app (0x10000) |
| `reset` | reset the main board into its firmware |
| `dl` | reset it into ROM download mode and leave it there |
| `report` | print the Sidecar's bus-sampler histograms now |

On the bench pair: app (971 KB) in 16 s at 2 Mbaud (erase 1.7 s; the ROM
protocol waits for each 1 KB block), all four images in about 18 s. A rate
the wires can't carry is caught by a register read right after the switch,
and the flasher starts over at 921600.

## Software changes required

### Before the main board can run (blocking): the R/W regroup

Everything that treats `r0 >> bus.addr.shift` as the address, or R/W as bit
17, must change. Today's users:

- **`main/pinDefs.h`**: `Pin<1,16> addr` and `Pin<17> rw` become A0–A13 on
  GPIO1–14, R/W on GPIO15, A14–A15 on GPIO16–17. Add helpers to convert
  between `r0` and an address in both directions (`r0ToAddr()`,
  `addrToR0()`), constexpr so the pre-shifted constants stay free.
- **`main/core1.cpp`** (and its csim mirror, `csim_bus_cycle()`, which must stay
  in step):
  - the head: `l1BlockSelShift`, `pageSelShift`, `pageInL1BlockSelBits` and
    the R/W splice (lines ~39 and ~360–362) become `extui(r0, 16, 2)` and
    `extui(r0, 9, 7)`;
  - `addr = r0 >> bus.addr.shift` (lines ~129, ~378): the low byte (A0–A7)
    is still `r0 >> 1`; the page number for `lastPageOffset[page]` needs the
    helper, or `lastPageOffset[]` indexed by `extui(r0, 9, 9)` with core 0's
    `pageD5` lookups (`main.cpp` ~210, ~252, ~1187) using the same mapping;
  - csim's fake `GPIO_IN_REG` (line ~99) builds `r0` with the helper.
  - The read/write branch tests `bus.rw.mask`, so it follows `pinDefs.h`.
- **`main/main.cpp`**: core 0's bmon decode (~470:
  `addr = (r0 & bus.addr.mask) >> shift`) and the debug dumps (~987, ~1116).
  The address constants it compares with (`_0xd830`, `_0xd1ff` ... in
  `const.h`) are plain addresses: they stay as they are once the decode uses
  the helper.
- **`main/pbi.cpp`** (~697): the stack-program wait's address decode
  (`PBIROM_DUMMY_WRITE`).
- **`main/bmon.h`**: the bmon trigger/exclude masks and values, built as
  `(0xf400 << bus.addr.shift)` etc.
- **`main/fakeBus.cpp`**: the FAKE_CLOCK bench drives the address pins as a
  contiguous range (`bus.addr.pin + bit`) and R/W as `bus.rw.pin`.
- **`csim/a800/csim_stubs.cpp`**: bmon entries built as `addr << bus.addr.shift`.
- **`main/profile.cpp`**: takes `bus.rw.pin` and `bus.addr.pin` (A0), so it
  follows `pinDefs.h`; check the GPIO-mode channel list once.

Verify in csim (`BUS_CYCLE=1 csim/a800/run.sh`, plus the BANKTEST configs),
then the bench (`FAKE_CLOCK`, `FAKE_SIO`), then `tools/core1_cycle_count.py`
and `PROFILE_MODE=PIN` to confirm the three cycles, then a soak.

The old espBusBang boards keep the old layout, so the code needs a build
option (a board type in `pinDefs.h`) rather than a switch-over, until those
boards retire.

### For J8/J9 (the sidecar_pcb_xl layout)

A board type in `pinDefs.h`: `refresh_` on GPIO20 (so `HAVE_REFRESH` holds
in Sidecar builds too) and an open-drain Atari reset on GPIO19, through R1.
The main board's console is then the link, not USB: the Sidecar should relay
it. With J8/J9 open, those pins must stay unused (they're the USB port).

### To use the link (with J3 on 2–3)

- Main app: build with `CONFIG_A8BB_SIDECAR_ENABLE` (host link pins GPIO0,
  GPIO21, GPIO48, already in `pinDefs.h`). Sidecar builds already drop
  `HAVE_REFRESH`, which is right: GPIO21 is the link's RX.
- Sidecar app: the link pins (TX 46, RX 9, BCLK 10) are already in
  `sidecar/main/sidecarApp.cpp` (9a354d1e).
- Without the Sidecar (J3 on 1–2): a normal build, with `HAVE_REFRESH`. Its
  status LED on GPIO48 now only toggles `LINK_BCLK` into the Sidecar's GPIO10,
  an input: harmless.

### Not yet written (the hardware is ready for them)

- **Bus profiler on the Sidecar** (`components/bus_profiler` on its free
  core 0, short masked chunks): inputs GPIO4 `PHI2`, 5 `READ`, 6 `EXTSEL`,
  7 `D0`, 15 `D1`, 16 `A0`, 17 `REFRESH`. No profile-pin channel (GPIO48 is
  BCLK).
- **Audio on GPIO47** (LEDC PWM or sigma-delta): e.g. a tick per sector I/O.
- **Serial flasher in the Sidecar app** (Espressif's `esp-serial-flasher`
  library does the esptool protocol from a host MCU): pulse `MAIN_EN` with
  GPIO9 held low, then UART1 on GPIO12/13; images from SPIFFS or `/sd`. Must
  refuse unless the Atari is off.
- **Disk images on the SD card**: the Sidecar mounts the card at `/sd`
  (82fb37e9); a diskSpec of `/sd/name.atr` already resolves there.

### Nothing to do

- GPIO0 leaving PBI audio and GPIO48 leaving R1 need no code: nothing drives
  PBI audio or RESET from the main board today.
