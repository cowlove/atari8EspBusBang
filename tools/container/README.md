# Reproducible ESP-IDF Build Environment

This Podman setup recreates the ESP-IDF source and toolchain currently used for
the firmware. It deliberately pins the IDF superproject commit, the 14 local
submodule commits selected for the reference build, the SPIFFS source patch,
and the managed-component lockfile. The setup script also fetches the project's
ignored `libsmb2` component at its pinned commit into the user's cache and
mounts it read-only; the source checkout is not modified to supply this
dependency.
The image builds pinned `xa` 2.4.1 (the Makefile expects its `-P` option and
legacy `~/bin/xa` path) and includes system NumPy for ROM-header generation. It
does not use a floating `v5.5` image/tag.

The container is a build environment only. It does not pass through serial
devices, flash the ESP32, publish files, or run the Atari image-generation and
hardware-control scripts.

## Atari-side development tools

The Podman image above covers the ESP32 firmware build, not every auxiliary
tool used by the Atari software and SPIFFS-image workflow. `tools/spiffs_flash.sh`
also expects LLVM-MOS target drivers for standard Atari cartridges, the
project's AtariMax 128 cartridge target, and Atari DOS executables. The
AtariMax target is a small SDK overlay (target config, banked-call support,
linker script, and runtime library), not part of stock LLVM-MOS. Its installer
and pinned SDK download are maintained in the companion
[`atarimax-bank-test` project](https://github.com/cowlove/atarimax-bank-test):

```sh
git clone https://github.com/cowlove/atarimax-bank-test.git
cd atarimax-bank-test
./scripts/install-llvm-mos-sdk.sh "$HOME/opt/llvm-mos-atari8"
```

That installs the official LLVM-MOS SDK v23.3.0 and applies the overlay at the
specified prefix. It is separate from the ESP-IDF container and does not
install over the default `~/opt/llvm-mos` path. Important: the previously used
development compiler reports LLVM-MOS 24.0.0git at commit
`cd9145597347753c7cabd1c34d24722d4d8a8f96`, so the v23.3.0 helper is a
reproducible clean installation, but is not yet proven equivalent to the
known-good Atari toolchain. Test it under the separate prefix before
considering any replacement.

The pinned ESP-IDF environment already supplies `esptool.py` (currently
v4.12.0 in this image) and the SPIFFS image generator at
`$IDF_PATH/components/spiffs/spiffsgen.py`; these do not need a separate
Arduino SDK installation. The existing `tools/spiffs_flash.sh` still calls an
old Arduino-tree `mkspiffs` binary, but IDF's generator accepts the same key
image parameters. For this script's 0x420000-byte image, 4096-byte blocks, and
256-byte pages, the candidate replacement is:

```sh
python "$IDF_PATH/components/spiffs/spiffsgen.py" \
  0x420000 ./main/lfs ./main/spiffs.bin \
  --block-size 4096 --page-size 256
```

Before changing the flashing script, compare/boot-test the generated image
against the firmware's SPIFFS settings; matching geometry alone does not prove
identical image behavior. The remaining external Atari tools are `atr`
utilities to edit ATR disk images and the Atari800 project's `cart` converter
to package a raw ROM as a Type 41 cartridge. Their current paths are
machine-specific and are not yet pinned here. Also, `tools/spiffs_flash.sh`
powers off the Atari and flashes the ESP32, so do not run it as part of a clean
container build test.

## Requirements

- Linux with rootless Podman configured
- Git and network access during the image build
- About 8 GB of free space for the base image, IDF tools, and build output

## Commands

From the project root:

```sh
./tools/container/setup.sh image
./tools/container/setup.sh build
# Or select a different hardware calibration ID (defaults to 4):
TEST_HW_ID=3 ./tools/container/setup.sh build
./tools/container/setup.sh shell
```

`image` creates the pinned toolchain image. `build` runs `idf.py build` with
the current project directory mounted into the container and passes the
required `TEST_HW_ID` CMake setting (default `4`; override with the environment
variable, valid values are 0 through 6). `shell` opens a container shell with
the ESP-IDF environment exported. Build output stays in
the project’s ignored `build/` directory; the build command also generates the
ignored `main/gitVersion.h` and `main/pbirom.h` files required by this checkout.
No host ESP-IDF installation is used or modified.

On SELinux-enforcing systems, add a private relabel suffix (`:Z`) to the two
volume arguments in `setup.sh`, or use an SELinux-compatible private source
copy for the trial build.

## Updating the frozen environment

Do not update these pins as routine maintenance. First compare the resulting
build against the frozen image and run the hardware timing soak separately.
To intentionally test newer IDF or submodule revisions, change the pins in
`Containerfile` and `idf-submodule-overrides.txt` in a separate change and
rebuild the image. The SPIFFS patch should remain explicit until it is either
upstreamed or no longer required.
