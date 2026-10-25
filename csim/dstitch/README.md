# FUSE SMB Disk Simulator

This directory contains a Linux FUSE filesystem that presents a virtual Atari
disk image backed by local files or an SMB share. Atari800 mounts that virtual
image as its second disk drive. The DOS 2.0 image is the default; SDFS is
selected explicitly for the read-only prototype.

## Prerequisites

On Debian or Ubuntu, install the compiler, CMake, pkg-config, FUSE 3 development
files and runtime utilities:

```sh
sudo apt install build-essential cmake pkg-config libfuse3-dev fuse3
```

You also need:

- A working Atari800 executable with XL/XE and hard-disk support. The
  `atari800` executable must be on `PATH`.
- The `atr` disk-image utility must also be on `PATH`; `run.sh` uses it to
  inspect and modify the test ATR image.
- A local SMB server running and serving a share named `foo`. The simulator
  connects to `smb://localhost/foo`; ensure the account used by the process can
  access that share. This code currently assumes the share name `foo`.
- Locally supplied Atari XL/XE and BASIC ROM images, an SDX cartridge image,
  and `fuse_d1.atr` (the test ATR used as drive 1). These binary assets are not
  distributed in this repository. `run.sh` defaults `SDX_CART` to
  `../../spiffs/SDX450_maxflash1.car`; set `SDX_CART` to your own cartridge
  image path if it is elsewhere. Atari800 must be configured to find your ROMs.

## Build

From the project root, initialize the `libsmb2` submodule:

```sh
git submodule update --init csim/dstitch/libsmb2
```

Build its shared library:

```sh
cd csim/dstitch/libsmb2
cmake -S . -B build -DBUILD_SHARED_LIBS=ON
cmake --build build --parallel
```

Build the FUSE executable from `csim/dstitch`:

```sh
cd ..
make
```

The Makefile gets FUSE 3 compiler and linker flags from `pkg-config fuse3` and
links `libsmb2/build/lib/libsmb2.so` directly. `run.sh` sets the standard
`CDEFS=-DLOG=printf -DSECTOR_SIZE=256` when `CDEFS` is unset. Override it by
setting `CDEFS` in the environment, for example:

```sh
CDEFS='-DLOG=printf -DSECTOR_SIZE=128' ./run.sh
```

## Run manually

Start the FUSE server in one terminal from this directory. Select the backing
resource with `file://<directory>` or `smb://<url>`. The image format defaults
to DOS 2.0; pass `--sdfs` to select SDFS or `--dos20` to select DOS 2.0
explicitly. `proc:` remains a standalone virtual image resource. The mountpoint
must exist, and the shared `libsmb2` library must be on the runtime linker path:

```sh
mkdir -p mnt h1
LD_LIBRARY_PATH="$PWD/libsmb2/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ./dstitch_mount smb://localhost/pub -f ./mnt
```

Leave it running in the foreground. In another terminal, also from this
directory, launch Atari800:

```sh
atari800 -xl -stretch 3 -cart "$SDX_CART" \
  ./fuse_d1.atr ./mnt/fuse_d2.img -turbo -dump-screen-after 900
```

For a local-directory-backed image, use `file://`. The default format is DOS
2.0; `--sdfs` selects the SDFS image. Format selection
and local backend selection are independent. These simulator options exist only
in this executable; firmware `main.cpp` is not wired to the SDFS image class:

```sh
mkdir -p mnt
./dstitch_mount file://./hostdir -f ./mnt
./dstitch_mount --dos20 file://./hostdir -f ./mnt
./dstitch_mount --sdfs file://./hostdir -f ./mnt
./dstitch_mount --sdfs --sdfs-sectors 32768 file://./hostdir -f ./mnt
./dstitch_mount --sdfs --sdfs-write-poc file://./hostdir -f ./mnt
```

For SDX create/extend write-order tracing, add `--sdfs-trace-overlay`. This
accepts all SDFS sector writes into a temporary RAM overlay, logs sector
sequence and before/after changed bytes, and serves later reads from that
overlay. It never writes those changes to the backing store; the overlay is
discarded when the mount exits. This mode is observational and does not infer
file ownership for unallocated sectors.

For the experimental create/append write POC, use `--sdfs-write-poc` instead of
`--sdfs-trace-overlay`. It buffers writes to unallocated sectors provisionally
and assigns them only when changed dirent and matching map-sector contents agree
on their ownership and order. The final dirent length identifies the valid
suffix of a partial-tail sector. The trace overlay and write POC share one
sector-keyed pending-write queue: unowned writes,
partial-tail sectors, and writes during `0x88` replacement remain there until
metadata resolution can commit them. First-write order supports hypothesis
checks without maintaining duplicate payload buffers. Current scope is new 8.3
files, append-only extension across linked map sectors, deletion of a
known file after its dirent is marked deleted, and the observed in-place
overwrite sequence (`0x08` → `0x88` → `0x08`). During that replacement the old
host contents and ownership remain committed; sector writes are staged until
the active dirent, final map chain, and all claimed payloads are available.
Rename is also supported in the write POC: a name-only change to a stable active
dirent invokes the backing `StorageInterface::rename` and updates inventory.
Sector 1 and the volume bitmap
are SDX-owned allocator scratch space: the POC passes their writes through but
does not use them to determine file ownership or gate create/append/delete/replace.
The POC follows the map chain and waits for late data payloads before committing,
including when a new map sector is identified through its predecessor's link.
After successful backing writes or a delete tombstone, the POC updates its own
in-memory file/map ownership and discards resolved transaction state.
`run.sh -h --sdfs-write-poc` selects this mode. Treat it as experimental and
test against a disposable backing directory.

SDFS stores sector addresses and the volume sector count in 16 bits, limiting a
volume to 65,535 sectors. That maximum is already the default geometry (about
16 MiB with 256-byte data sectors). Set a smaller image with
`--sdfs-sectors N`; `run.sh` forwards the option, for example
`./run.sh -h --sdfs-sectors 32768`. The option accepts 64 through 65,535 and
requires `--sdfs`.

Then launch Atari800 directly with the command shown above. This prototype currently requires
256-byte data sectors and strict 8.3-compatible host filenames. Unsupported or
colliding names make image initialization fail. It is read-only; disk writes
return write-protected.

Run the sector-level smoke test (no emulator or FUSE mount required) with:

```sh
make sdfs_test
./sdfs_test
```

The test harness launches Atari800 directly (under `xvfb-run` for headless
runs) and redirects its stdout and stderr to `atari800.log`. Stop the FUSE
process with Ctrl-C when finished; if needed, unmount with
`fusermount -u ./mnt` (some systems provide this command as `fusermount3`).

## Test harness

`run.sh` combines build, ATR setup, Atari800 launch, FUSE mount, and backing
file comparisons. It polls `fuse_d1.atr` once per second for a `done` file and
stops the emulator and FUSE mount when it appears. It also exits when either
child exits, or after the optional `-t <sec>` timeout. Ctrl-C follows the same
PID-scoped cleanup path:

```sh
./run.sh --headless
./run.sh -t 30
./run.sh --sdfs-write-poc
```

Use `-h` or `--headless` to run Atari800 under `xvfb-run -a`; `xvfb-run` must be
installed. `--help` prints all options. The optional positional URL overrides
the default share. `CDEFS` can be overridden as shown in the build section
above. The FUSE filesystem exposes the virtual disk as `fuse_d2.img`.

**Caution:** `run.sh` unmounts `./mnt`, clears `hostdir` and `expected`, recreates
`fuse_d1.atr`, and runs `make clean` before rebuilding `dstitch_mount`. Ensure
those test assets are disposable before running it. Its cleanup targets only
the child processes started by that invocation.
