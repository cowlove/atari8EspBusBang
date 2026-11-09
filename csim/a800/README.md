# Atari800 CSIM hybrid

The hybrid builds Atari800 from the `atari800` Git submodule in this directory. It configures Atari800 in-place; no separate Atari800 build directory is needed.

## Initialize the submodule

From the `atari8EspBusBang` repository root:

```sh
git submodule update --init --recursive
```

The submodule tracks Jim's `cowlove/atari800` fork on the `csim-pbi` branch.

## Generate and configure Atari800

The submodule checkout may not contain the generated `configure` script. From `csim/a800/atari800`:

```sh
./autogen.sh
CFLAGS='-g -O2 -Wno-error=unused-function' \
  ./configure --with-video=sdl2 --with-sound=sdl2 \
  --disable-audiorecording --disable-dependency-tracking
```

This is the SDL2 configuration (`VIDEO=1`). `autogen.sh` requires the autotools utilities (`aclocal`, `autoheader`, `autoconf`, and `automake`); SDL2 development files are needed for this configuration. Run both commands from the submodule root so configure creates `src/Makefile` there.

If `configure` or `src/Makefile` is missing, the Makefile generates/configures it automatically. The Makefile defaults to `VIDEO=0` (no video or sound); use `VIDEO=1` to enable SDL2:

```sh
make -C csim/a800 VIDEO=1 hybrid
```

Switching `VIDEO` automatically reconfigures Atari800 and reuses the same build and executable directories. `VIDEO=0` is already the default; use `make -C csim/a800 VIDEO=1 hybrid` for SDL2. `run.sh` launches the built emulator directly without Xvfb. If also profiling, combine variables, for example `make -C csim/a800 VIDEO=1 PROFILE=1 hybrid`.

## Build and run

From the repository root:

```sh
make -C csim/a800 hybrid
csim/a800/run.sh
```

`BOOT_CONFIG` selects the firmware boot configuration and defaults to `CSIM`.
Set it to any configuration name supported by `SysConfig`, for example:

```sh
BOOT_CONFIG="SDX_XL" make -C csim/a800
```

Changing `BOOT_CONFIG` automatically rebuilds the CSIM objects that embed the
selected configuration.

To start Atari800 under GDB, with `-xl -nopatchall` and any following arguments passed to the emulator:

```sh
csim/a800/run.sh --gdb -turbo
```

The hybrid executable is written to `csim/a800/build-atari/atari800-csim`. Additional arguments to `run.sh` are forwarded to Atari800.

CSIM uses the real `libsmb2` client from the shared `csim/libsmb2` submodule
for SMB-backed disks. Initialize it with `git submodule update --init
csim/libsmb2`; the CSIM Makefile builds its shared library in the
separate ignored `csim/a800/build-libsmb2` directory and links both runner and hybrid
targets against it. SMB URLs must be reachable from the host running CSIM, and
the URL or configured credentials must permit access.

## CSIM scope and emulator services

CSIM runs the firmware-side code in a host process; by default it does not
itself emulate Atari hardware memory mapping (see bus-cycle mode below for
the exception). Keep the normal firmware paths compiled and
running under CSIM—do not exclude them with `#ifdef CSIM` just because their
hardware services are not implemented by the CSIM stubs. Atari800 supplies
the guest-system behavior, including cartridge mapping, extended-memory
mapping, and RAM-under-OS behavior, according to its startup configuration.

In particular, `run.sh` starts Atari800 with `-xl -nopatchall`, the
`SDX451_maxflash1.car` cartridge, `-turbo`, and `-rambo`. `-rambo` enables
Atari800's RAMBO extended-memory model; SDX's `RAMDISK.SYS` driver still has to
be installed by SDX (for example, from its configuration or a batch command)
to create a DOS-visible RAM disk. `BOOT_CONFIG` selects the firmware
configuration independently of the cartridge passed to Atari800 by `run.sh`,
so those two selections can differ.

## Bus-cycle mode (`BUS_CYCLE=1`)

`BUS_CYCLE=1 csim/a800/run.sh` builds both sides with `-DCSIM_BUS_CYCLE`: every
CPU access in atari800 runs one bus cycle through core 1's loop body
(`iloop_pbi_cycle()` in `main/core1.cpp`), so the board's own memory map
(`mmu.cpp`, `cartridge.cpp`, extended banks, PBI ROM) serves the 6502. When
the body asserts extSel_ the board handled the access; otherwise atari800
does. `run.sh` then defaults `BOOT_CONFIG` to `SDX_CSIM_BUS` and gives
atari800 no cartridge and no `-rambo`, since the board supplies both.
`CSIM_BUS_CYCLE_TEST=1` runs a short self-test at startup. Expect it to run
several times slower than the default mode.

What it doesn't cover:

- **Halt and PSRAM swaps.** The board halts the 6502 on a PORTB or $D1FF
  write while core 0 pages a PSRAM bank into SRAM and patches the MMU. Only
  the hardware can halt: atari800 doesn't stall when the body sets halt_ in
  csim's GPIO variables, and core 0 (`mmuOnChange()`) runs from the CPU tick,
  16 or more cycles later. So csim doesn't evict at all:
  `ExtBankPool::isSRAM()` passes every host pointer, every bank counts as
  SRAM, and the status line reads `evict 0/N`. Test evictions on hardware.
- **ANTIC.** Its DMA reads use atari800's memory, not the board's map, so
  the display shows atari800's RAM where the board serves memory.
- **Timing.** The body's delays are no-ops; the cycle order is atari800's.
  atari800's monitor history peeks at the code without a bus cycle, so the
  board sees only the CPU's own accesses.

### banktest

`spiffs/banktest.c` (`make -C spiffs banktest`, boot config `BANKTEST`)
checks that a bank switch reaches the very next bus cycle: an STA to PORTB
or $D5xx at the end of RAM falls through into the banked window, and bank B
counts the hit in X. It passes in the default mode, where atari800 switches
banks itself, and reports the board's misses in bus-cycle mode:

```sh
CART=../../spiffs/.build/banktest.car BOOT_CONFIG=BANKTEST csim/a800/run.sh
BUS_CYCLE=1 BOOT_CONFIG=BANKTEST csim/a800/run.sh
```

Today the board misses every PORTB switch (it maps the bank at the end of
the next read) and none of the AtariMax $D5xx switches (the write cycle
itself selects the bank). On hardware, `BANKTEST` uses one SRAM bank, so
every PORTB switch also exercises the halt and PSRAM swap.

## Profiling

Build both the CSIM code and Atari800 with GNU `gprof` instrumentation by setting `PROFILE=1`:

```sh
make -C csim/a800 PROFILE=1 hybrid
```

This adds `-pg` to the CSIM compiler and linker flags and to Atari800's `CFLAGS`, `CXXFLAGS`, and `LDFLAGS`. The normal build remains the default (`PROFILE=0`); switch back with `make -C csim/a800 PROFILE=0 hybrid`. Changing the profile setting automatically refreshes affected objects and Atari800 configuration, while keeping the same build and executable directories.

## Sidecar

CSIM always runs the Sidecar coprocessor app (`sidecar/main/sidecarApp.cpp`)
in the same process: `csim.h` defines `CONFIG_A8BB_SIDECAR_ENABLE`, so the
host's Sidecar code (`main/sidecarHost.cpp`) is on too. The two meet in a
simulated i2slink (`components/sidecar_link/i2slink_csim.cpp`) that replaces
the I2S hardware at the `i2slink.h` boundary:

- each direction is a shared buffer: `i2slink_write()` builds the real wire
  cycle (VSYNC, HSYNC markers, stamps, CRC) and stores it in the peer's RX
  ring, bit-shifted by a random amount per connection, so the reader's real
  lock search and decode (`i2slink_wire.h`) run;
- `csim_sidecar.cpp` wires the host's link pins (virtual GPIOs 100/101 in
  `pinDefs.h`) to the Sidecar's, and polls the Sidecar's loop after every
  link write (so a response is ready when the host's write returns) and on
  every emulated CPU tick;
- there is no timing model: `read()` never waits, and a wait only advances
  simulated time (`esp_timer_get_time()` follows CCOUNT).

At startup the host probes the Sidecar and sends one SIO command; look for
`Sidecar: present; SIO echo ok` in the output.
