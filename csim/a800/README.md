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

To start Atari800 under GDB, with `-xl -nopatchall` and any following arguments passed to the emulator:

```sh
csim/a800/run.sh --gdb -turbo
```

The hybrid executable is written to `csim/a800/build-atari/atari800-csim`. Additional arguments to `run.sh` are forwarded to Atari800.

## Profiling

Build both the CSIM code and Atari800 with GNU `gprof` instrumentation by setting `PROFILE=1`:

```sh
make -C csim/a800 PROFILE=1 hybrid
```

This adds `-pg` to the CSIM compiler and linker flags and to Atari800's `CFLAGS`, `CXXFLAGS`, and `LDFLAGS`. The normal build remains the default (`PROFILE=0`); switch back with `make -C csim/a800 PROFILE=0 hybrid`. Changing the profile setting automatically refreshes affected objects and Atari800 configuration, while keeping the same build and executable directories.
