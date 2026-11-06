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

CSIM runs the firmware-side code in a host process; it does not itself emulate
Atari hardware memory mapping. Keep the normal firmware paths compiled and
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
