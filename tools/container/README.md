# Reproducible ESP-IDF Build Environment

The shared [`setup-build-env.sh`](setup-build-env.sh) installs the pinned
ESP-IDF source/toolchain and LLVM-MOS SDK support either directly on a native
Linux host or inside the Podman image. It pins the IDF superproject commit, the
14 submodule commits selected for the reference build, and the SPIFFS source
patch. The Podman build invokes its `idf` and `llvm-mos` phases in separate
image layers so a later SDK-layer failure can reuse the completed IDF layer.
The same script fetches the project's ignored `libsmb2` component at its pinned
commit into `components/libsmb2` for native builds; the container wrapper runs
that setup phase on the host before mounting the project, so both build modes
use the same component checkout without baking project sources into the image.
The SDK setup
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
longer required. The flashing workflow uses the separately pinned
[`jhallen/atari-tools`](https://github.com/jhallen/atari-tools) `atr` utility
to edit ATR disk images. The optional `tools/atari800-*.sh` scripts still use
the Atari800 emulator.

`tools/spiffs_flash.sh` creates `d1.atr`, `d2.atr`, and `llvm_d1.atr` as blank
DOS 2.0 double-density (180 KB) disk images with `atr ... mkfs dos2.0d`, but
only when each image is missing. Existing images are preserved and get the
script's usual file updates.

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
identical image behavior. `tools/spiffs_flash.sh` powers off the Atari and
flashes the ESP32, so do not run it as part of a clean container build test.
It resolves the required LLVM-MOS executables from `LLVM_MOS_BIN`, or searches
the installed binary directories under `~/opt`; it does not require them to be
on `PATH`. Discovery also verifies the AtariMax 128 runtime library is present,
so a stock LLVM-MOS install with a similarly named driver is rejected.

## Native Linux setup

From the project root, run the installer as your normal user (not with `sudo`):

```sh
./tools/container/setup-build-env.sh
```

On Debian/Ubuntu, it installs any missing system prerequisites through `sudo`
and then installs the pinned ESP-IDF, LLVM-MOS SDK, Atari disk tools, and the
project's pinned `libsmb2` component into the ignored `components/libsmb2`
directory.
`sudo` may prompt for your
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
export PATH="$HOME/opt/atari-tools/bin:$LLVM_MOS_BIN:$PATH"
idf.py -DHW_TYPE=\\"4\\" build
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

`container.sh` checks its host prerequisites and installs missing packages on
Debian/Ubuntu through `sudo apt-get`. The runtime setup includes Podman, crun,
`passt`/`pasta`, `uidmap`, ccache, Git, `find`, and `usermod` support. You can install
them ahead of time with:

```sh
./tools/container/setup-build-env.sh container-runtime
```

This installs the `podman`, `crun`, `passt`, `uidmap`, `ccache`, `git`,
`findutils`, and `passwd` packages through `sudo apt-get` if needed. `passt`
provides Podman's rootless `pasta` network helper; Git and `find` support
source/component discovery, `passwd` provides `usermod` for subordinate-ID
setup, and host ccache reports its configured cache path. Standard
base utilities (such as `bash`, `coreutils`, `grep`, and `awk`) are also used.
The script requires `sudo` for package installation and privileged setup if
running as a normal user; install `sudo` manually if it is absent. It also
checks `/etc/subuid` and `/etc/subgid` for at least 65,536
subordinate IDs for the invoking user; when missing, it adds a non-overlapping
range using `usermod`. Run the script as your normal user (it uses `sudo` for
the privileged changes).
The firmware image does not need crun; Podman uses it on the host for the flash
invocation so the container can retain the caller's supplementary `dialout`
group. The flash helper will use crun for that invocation if another OCI
runtime is the configured default. Existing subordinate ID mappings are
preserved; a fresh 65,536-ID range is added only when the user lacks sufficient
capacity. Other rootless Podman configuration is left untouched.

## Commands

From the project root:

```sh
./tools/container/container.sh image
./tools/container/container.sh pull
./tools/container/container.sh build
# Pass idf.py options after the command; HW_TYPE defaults to "800XL" when omitted:
./tools/container/container.sh build -DHW_TYPE=\\"3\\"
./tools/container/container.sh shell
# Remove intermediate build products, or all generated build state:
./tools/container/container.sh clean
./tools/container/container.sh fullclean
# Flash the connected ESP32 over a specific serial device:
./tools/container/container.sh flash /dev/serial/by-id/usb-...
# Build the SPIFFS payload without flashing:
./tools/container/container.sh flash_spiffs
# Build only when inputs are newer than the image:
./tools/container/container.sh flash_spiffs --if-needed
# Build and flash the SPIFFS payload over a specific serial device:
./tools/container/container.sh flash_spiffs /dev/serial/by-id/usb-...
# Build and flash only when inputs changed:
./tools/container/container.sh flash_spiffs /dev/serial/by-id/usb-... --if-needed
# Disable host caches and rebuild image layers from scratch:
./tools/container/container.sh --no-cache image
```

`flash_spiffs [device] [options...]` runs `tools/flash_spiffs.sh` in the container
and forwards its options. When a device is given, it passes `-p <device>` and
flashes; without one it only builds. The optional `--if-needed` (or `-u`) skips
both build and flash when `spiffs.bin` is already up to date according to the
Makefile dependencies.
The script runs `make -C spiffs` to compile `stress.c` as an AtariMax 128 cartridge,
wrap it as `stress.car`, generate `spiffs/spiffs.bin` from the `.car`, `.rom`,
and `.atr` files in `spiffs/`, then flash that image at the SPIFFS partition
offset (`0x3D0000`). The run container has networking disabled; all required
tools are already in the image. No physical-device flash was run as part of
testing this target. Run `tools/flash_spiffs.sh` without `-p` to build the
image without flashing; `-p <device>` is optional for that standalone command.

For maintainers publishing a locally built image to GHCR, first create the
package or ensure the account can publish it, then authenticate interactively
and push:

```sh
podman login ghcr.io
./tools/container/container.sh push
```

Use `pull <tag>` and `push <tag>` to select a release tag other than the
default. Set `CONTAINER_REGISTRY_IMAGE` to change the registry/repository.

`image` locally creates the pinned toolchain image. `pull` fetches the
published image and tags it with the same local name, so the existing `build`,
`clean`, `fullclean`, `flash`, and `shell` commands can use either source. The
default registry reference is
`ghcr.io/cowlove/atari8espbusbang-idf:5.5-02c5f2d-llvm-mos`; override it with
`CONTAINER_REGISTRY_IMAGE` and `CONTAINER_IMAGE_TAG` when needed. Maintainers
can publish the local image with `container.sh push [tag]` after logging into
the registry with Podman. For a release, build and test with `image`, then
`push`; consumers can run `pull` before the normal build command. The registry
package must be public for anonymous pulls. Pulling a release tag gives users
the exact published image artifact; independently rebuilding the Containerfile
uses the same pinned project tool sources but can differ as Debian base/package
versions change.

By default, `image` searches
under `$HOME` for an ESP-IDF Git checkout containing the pinned ESP-IDF commit
and required submodule commits, and uses it as a local Git object cache. It
also uses an existing `$IDF_TOOLS_PATH/dist` (default
`$HOME/.espressif/dist`) as the ESP-IDF download cache. The checkout is mounted
read-only for the image build; the dist cache is mounted read/write at
`/opt/espressif/dist`, so downloaded archives persist on the host without
being copied into the image. Set `IDF_SOURCE_CHECKOUT` or `IDF_DIST_CACHE` to
select specific cache paths. The host checkout is not modified. `--no-cache`
disables the host source/dist cache mounts and forces Podman to rebuild image
layers; it can precede any container command, for example
`container.sh --no-cache build`.

`clean` and `fullclean` run `idf.py` inside a one-shot container against the
same persistent `.container-build` directory used by `build` and `flash`.
`clean` removes ordinary build products; `fullclean` removes the generated
build state, including stale CMake generator caches (for example, a leftover
Unix Makefiles bootloader cache when the current build uses Ninja). Both accept
an optional project directory, like `build` and `shell`. They leave source
files and the rest of the project tree untouched.

For native setup, `setup-build-env.sh` similarly searches under `$HOME` for an
ESP-IDF checkout containing the pinned Git objects and uses it for source and
submodule fetches. It uses `$IDF_TOOLS_PATH` (default `$HOME/.espressif`) for
IDF tools and downloaded archives. `setup-build-env.sh --no-cache` disables
the local Git checkout cache. Because `IDF_TOOLS_PATH` is also the persistent
installation directory for IDF tools, its existing `dist` contents remain in
use; to isolate native setup from that directory, set `IDF_TOOLS_PATH` to a
new empty persistent directory and keep that setting when sourcing `export.sh`.
Neither form of `--no-cache` deletes or modifies existing caches.

An existing checkout is not automatically sufficient just because its
submodules are initialized: this project uses several submodule revisions
different from ESP-IDF's recorded gitlinks. Its submodule repositories must
also contain those exact override commit objects. The current known-good
checkout at `$HOME/esp/esp-idf` has them.

`build` runs `idf.py build` with the current project directory mounted into the
container and passes any remaining command-line arguments to `idf.py` before
the `build` subcommand. For example, pass `-DHW_TYPE=\\"3\\"` to select a hardware
timing profile; valid values are the strings `"1"` through `"6"`, or `"130XE"`,
`"600XL"`, and `"800XL"` (aliases for profiles 5, 3, and 1). If omitted,
`main/core1defs.h` defaults to `"800XL"`. An optional project directory may precede the `idf.py`
arguments. `flash` accepts optional `idf.py` arguments in the same way, after
its required serial device and optional project directory. Both build and
flash enable ESP-IDF's `--ccache` by default; pass `--no-ccache` among the
forwarded `idf.py` options to disable it for that invocation. `shell` opens a
container shell with the ESP-IDF environment exported and ccache enabled for
`idf.py`. Build output stays in
the project's ignored `.container-build/` directory; the build command also generates the
ignored `main/gitVersion.h` and `main/pbirom.h` files required by this checkout.
No host ESP-IDF installation is used or modified.

The image contains its own `ccache` executable. The host ccache executable is
used to query the host's configured `cache_dir`; each build, flash, and shell
run mounts that directory at `/root/.cache/ccache` in the one-shot container,
so entries survive `podman run --rm`. Set `ATARI8ESP_CCACHE_DIR` to override
the host cache path explicitly. The mounted cache is separate from Podman
image-layer caching; `--no-cache` controls image builds, not ccache.

`flash` is separate and opt-in. A `/dev/serial/by-id/...` path is recommended;
the script checks permissions on its resolved host device node but preserves
the by-id path inside the container and passes only that character device using
Podman's `--device`. Other device paths are resolved to their host node. For
rootless Podman, USB serial passthrough
also requires the `crun` OCI runtime so `--group-add keep-groups` can preserve
the host user's device-group access; the script selects `crun` for this one run
if another runtime is configured. It refuses to broaden access with
`--privileged`. It runs `idf.py flash` and may build before flashing. The normal
`build` command never receives hardware access. On Ubuntu, serial devices are
often accessible to members of the `dialout` group; log out and back in after
changing group membership. Confirm the device belongs to the intended ESP32
before invoking `flash`.

On SELinux-enforcing systems, add a private relabel suffix (`:Z`) to the
project and ccache volume mounts in `container.sh`; host ESP-IDF source/cache
mounts may also need SELinux-compatible labeling or a private copy.

## Updating the frozen environment

Do not update these pins as routine maintenance. First compare the resulting
build against the frozen image and run the hardware timing soak separately.
To intentionally test newer IDF or submodule revisions, change the pins in
`setup-build-env.sh` and `idf-submodule-overrides.txt` in a separate change,
then rebuild the image. The SPIFFS patch should remain explicit until it is
either upstreamed or no longer required.
