# Reproducible ESP-IDF Build Environment

The shared [`setup-build-env.sh`](setup-build-env.sh) installs the pinned
ESP-IDF source/toolchain and LLVM-MOS SDK support either directly on a native
Linux host or inside the Podman image. It pins the IDF superproject commit, the
14 submodule commits selected for the reference build, and the SPIFFS source
patch. The Podman build invokes its `idf` and `llvm-mos` phases in separate
image layers so a later SDK-layer failure can reuse the completed IDF layer.
The runtime setup script also fetches the project's ignored `libsmb2`
component at its pinned commit into the user's cache and mounts it read-only;
the source checkout is not modified to supply this dependency. The SDK setup
downloads the official LLVM-MOS v23.3.0 prebuilt compiler, then builds and
installs SDK platform support from the pinned
[`cowlove/llvm-mos-sdk` AtariMax branch](https://github.com/cowlove/llvm-mos-sdk/tree/atari8-atarimax128-support).
This builds SDK runtime/platform libraries with the prebuilt MOS compiler; it
does not build LLVM/Clang. The image also includes system NumPy for ROM-header
generation and does not use a floating ESP-IDF `v5.5` image/tag. The common
script does not install host OS packages or use `sudo`; install the listed
prerequisites first.

The container is a build environment only. It does not pass through serial
devices, flash the ESP32, publish files, or run the Atari image-generation and
hardware-control scripts.

## Atari-side development tools

The SDK phase calls [`install-llvm-mos-sdk.sh`](install-llvm-mos-sdk.sh). It
starts with the checksum-pinned official v23.3.0 compiler archive, fetches the
exact source commit recorded in that script, and runs the SDK's CMake install
into a new prefix. This rebuilds platform support libraries across the SDK,
but not the LLVM-MOS compiler itself. Keeping the platform and regression
tests in the SDK fork gives us a clean upstreamable change; the companion
[`atarimax-bank-test` project](https://github.com/cowlove/atarimax-bank-test)
remains a consumer and regression harness rather than the source of the SDK
patch.

To install it outside the container, choose a fresh prefix (the script refuses
to overwrite an existing directory):

```sh
./tools/container/install-llvm-mos-sdk.sh "$HOME/opt/llvm-mos-atarimax128"
```

The new target is `mos-atari8-cart-atarimax128-clang`; its SDK fork branch adds
the AtariMax linker layout, banked-call runtime support, and compile/negative
compile tests. The installer runs those two tests before installation. This is
not yet a hardware validation or an upstream LLVM-MOS pull request.

The Podman image includes the standard Atari cartridge/DOS drivers from the
SDK as well. `tools/spiffs_flash.sh` writes the 16-byte Type 41 CART header
using `tools/make_cart.py`, which validates the 2–128 KiB ROM payload and
leaves the unused checksum field zeroed. The Atari800 `cart` utility is no
longer required. The flashing workflow still relies on the external `atr`
utility to edit ATR disk images; that dependency is not installed or pinned
here. The optional `tools/atari800-*.sh` scripts still use the Atari800
emulator.

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
identical image behavior. The remaining external Atari tool used by
`tools/spiffs_flash.sh` is the `atr` utility to edit ATR disk images. Its
current path is machine-specific and is not yet pinned here. Also,
`tools/spiffs_flash.sh`
powers off the Atari and flashes the ESP32, so do not run it as part of a clean
container build test.

## Native Linux setup

From the project root, run the installer as your normal user (not with `sudo`):

```sh
./tools/container/setup-build-env.sh
```

On Debian/Ubuntu, it installs any missing system prerequisites through `sudo`
and then installs both pinned environments. `sudo` may prompt for your
password in the terminal; the script uses your own `$HOME` for the toolchain
prefixes. On other distributions, install the equivalent packages yourself
and set `SKIP_SYSTEM_PACKAGES=1` when running the script.

The default native prefixes are `$HOME/opt/atari8esp-idf` and
`$HOME/opt/llvm-mos-atarimax128`; ESP-IDF's downloaded tools and Python
environment are cached under `$HOME/.espressif`. Existing install prefixes are
never overwritten. To choose different destinations, set `ESP_IDF_PREFIX`,
`IDF_TOOLS_PATH`, or `LLVM_MOS_PREFIX` when invoking the script. For recovery
or staged installation, `setup-build-env.sh idf` and
`setup-build-env.sh llvm-mos` run those phases separately; if the second phase
fails after IDF is installed, rerun just `llvm-mos`.

Activate the environment and build without flashing:

```sh
source "$HOME/opt/atari8esp-idf/export.sh"
export LLVM_MOS_BIN="$HOME/opt/llvm-mos-atarimax128/bin"
export PATH="$LLVM_MOS_BIN:$PATH"
idf.py -DTEST_HW_ID=4 build
```

The script downloads ESP-IDF and its pinned submodules/toolchain as well as the
LLVM-MOS release archive on first use, so the first setup can take a while. It
does not run the Atari image-generation or hardware-control/flash scripts. Set
`SKIP_SYSTEM_PACKAGES=1` to disable all package-manager actions if you have
already installed the prerequisites or want to manage them yourself.

## Podman requirements

- Linux with rootless Podman configured
- Git and network access during the image build
- About 8 GB of free space for the base image, IDF tools, and build output

## Commands

From the project root:

```sh
./tools/container/container.sh image
# Optional: reuse an existing, fully populated host ESP-IDF checkout and dist cache:
IDF_SOURCE_CHECKOUT="$HOME/esp/esp-idf" \
IDF_DIST_CACHE="$HOME/.espressif/dist" \
  ./tools/container/container.sh image
./tools/container/container.sh build
# Or select a different hardware calibration ID (defaults to 4):
TEST_HW_ID=3 ./tools/container/container.sh build
./tools/container/container.sh shell
# Flash the connected ESP32 over a specific serial device:
./tools/container/container.sh flash /dev/serial/by-id/usb-...
```

`image` creates the pinned toolchain image. By default, the image build clones
ESP-IDF and downloads tools as before. Optionally set `IDF_SOURCE_CHECKOUT` to
a host Git checkout and `IDF_DIST_CACHE` to an ESP-IDF `dist` directory. The
checkout is mounted read-only only during the image build; the dist cache is
mounted read/write at `/opt/espressif/dist` during the install layer, so
downloaded archives persist on the host without being copied into the image.
The source checkout must be at the exact pinned ESP-IDF commit, have all
recursive submodules initialized, and contain every override commit listed in
`idf-submodule-overrides.txt`. The script validates these conditions before
using its Git repositories as local fetch sources. The host checkout itself is
not changed. The dist cache directory is created if absent; ensure it is
writable by Podman.

An existing checkout is not automatically sufficient just because its
submodules are initialized: this project uses several submodule revisions
different from ESP-IDF's recorded gitlinks. Its submodule repositories must
also contain those exact override commit objects. The current known-good
checkout at `$HOME/esp/esp-idf` has them.

`build` runs `idf.py build` with
the current project directory mounted into the container and passes the
required `TEST_HW_ID` CMake setting (default `4`; override with the environment
variable, valid values are 0 through 6). `shell` opens a container shell with
the ESP-IDF environment exported. Build output stays in
the project’s ignored `.container-build/` directory; the build command also generates the
ignored `main/gitVersion.h` and `main/pbirom.h` files required by this checkout.
No host ESP-IDF installation is used or modified.

`flash` is separate and opt-in. It resolves the supplied host device path (a
`/dev/serial/by-id/...` symlink is recommended), checks that the host user can
read and write it, and passes only that character device into the one-shot
container with Podman's `--device`. It runs `idf.py flash` and may build before
flashing. The normal `build` command never receives hardware access. On Ubuntu,
serial devices are often accessible to members of the `dialout` group; log out
and back in after changing group membership. Confirm the device belongs to the
intended ESP32 before invoking `flash`.

On SELinux-enforcing systems, add a private relabel suffix (`:Z`) to the
project volume in `container.sh`; host ESP-IDF source/cache mounts may also
need SELinux-compatible labeling or a private copy.

## Updating the frozen environment

Do not update these pins as routine maintenance. First compare the resulting
build against the frozen image and run the hardware timing soak separately.
To intentionally test newer IDF or submodule revisions, change the pins in
`setup-build-env.sh` and `idf-submodule-overrides.txt` in a separate change,
then rebuild the image. The SPIFFS patch should remain explicit until it is
either upstreamed or no longer required.
