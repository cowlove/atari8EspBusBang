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

This is the SDL2 video/sound configuration tested for the hybrid. `autogen.sh` requires the autotools utilities (`aclocal`, `autoheader`, `autoconf`, and `automake`); SDL2 development files are also needed. Run both commands from the submodule root so configure creates `src/Makefile` there.

The same configure options are the Makefile defaults. If `configure` or `src/Makefile` is missing, `make -C csim/a800 hybrid` generates/configures it automatically.

### No display and no sound

To configure a headless Atari800 without SDL or sound support, run this from the repository root:

```sh
cd csim/a800/atari800
CFLAGS='-g -O2 -Wno-error=unused-function' \
  ./configure --with-video=no --with-sound=no \
  --disable-dependency-tracking
cd ../../..
make -C csim/a800/atari800 clean
make -C csim/a800 hybrid
```

This reuses the same in-tree configuration/build location and produces the same `csim/a800/build-atari/atari800-csim` executable; `Makefile` and `run.sh` need no path changes. Re-run the SDL2 configure command above to switch back to video/sound.

## Build and run

From the repository root:

```sh
make -C csim/a800 hybrid
csim/a800/run.sh
```

For a headless run under Xvfb:

```sh
csim/a800/run.sh --headless
```

To start Atari800 under GDB, with `-xl -nopatchall` and any following arguments passed to the emulator:

```sh
csim/a800/run.sh --gdb -turbo
```

Combine `--gdb` and `--headless` to run GDB under Xvfb.

The hybrid executable is written to `csim/a800/build-atari/atari800-csim`. Additional arguments to `run.sh` are forwarded to Atari800.
