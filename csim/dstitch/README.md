# FUSE SMB Disk Simulator

This directory contains a Linux FUSE filesystem that presents a virtual Atari
disk image backed by local files or an SMB share. Atari800 mounts that virtual
image as its second disk drive. The DOS 2.0 image is the default; SDFS is
selected explicitly for the read-only prototype.

## Prerequisites

On Debian or Ubuntu, install the compiler, CMake, pkg-config, FUSE 3 development
files and runtime utilities, and an xterm:

```sh
sudo apt install build-essential cmake pkg-config libfuse3-dev fuse3 xterm
```

You also need:

- A working Atari800 executable with XL/XE and hard-disk support. The
  `atari800` executable must be on `PATH`.
- The `atr` disk-image utility must also be on `PATH`; `run.sh` uses it to
  inspect and modify the test ATR image.
- A local SMB server running and serving a share named `foo`. The simulator
  connects to `smb://localhost/foo`; ensure the account used by the process can
  access that share. This code currently assumes the share name `foo`.
- The local test assets: `REV03.ROM` (XL/XE ROM), `REVC.ROM` (BASIC ROM),
  `SDX450_maxflash1.car` (SDX cartridge image), and `fuse_d1.atr` (the test
  ATR used as drive 1). `atari800.sh` also uses `./h1` as its H:1 hard-drive
  directory. The ROMs and cartridge are in this directory; the ATR and `h1`
  are local test assets and may need to be supplied separately. Several of
  these artifacts are ignored by Git, so a fresh checkout may not contain them.

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
./atari800.sh
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
discarded when the mount exits. While a create dirent is in progress, it tags
new-sector writes with that file's name. It warns if multiple in-progress
file entries overlap and checks observed new-sector write order against the
later sector-map entries. The order check is observational evidence for the
current trace, not proof of SDX's behavior in every case. Ownership and sector
order validation are correctness requirements for the eventual writable path;
violations must be rejected rather than misattributed.

For the experimental new-file write POC, use `--sdfs-write-poc` instead of
`--sdfs-trace-overlay`. It stages an in-progress create in RAM and only creates
the backing file after the final dirent, volume update, and matching sector map
have been observed. Current scope is new 8.3 files fitting in one map sector;
existing-file extension, multi-map files, delete, and resize are not supported.
After successful backing writes, the POC commits validated metadata in memory
and discards that transaction's overlay and hypothesis list. `run.sh -h
--sdfs-write-poc` selects this mode. Treat it as experimental and test against
a disposable backing directory.

SDFS stores sector addresses and the volume sector count in 16 bits, limiting a
volume to 65,535 sectors. That maximum is already the default geometry (about
16 MiB with 256-byte data sectors). Set a smaller image with
`--sdfs-sectors N`; `run.sh` forwards the option, for example
`./run.sh -h --sdfs-sectors 32768`. The option accepts 64 through 65,535 and
requires `--sdfs`.

Then launch Atari800 with `./atari800.sh`. This prototype currently requires
256-byte data sectors and strict 8.3-compatible host filenames. Unsupported or
colliding names make image initialization fail. It is read-only; disk writes
return write-protected.

Run the sector-level smoke test (no emulator or FUSE mount required) with:

```sh
make sdfs_test
./sdfs_test
```

The launcher runs Atari800 directly (no `xterm` wrapper). The test harness
redirects Atari800 stdout and stderr to `atari800.log`. The launcher uses
`REV03.ROM`, `REVC.ROM`, `SDX450_maxflash1.car`, `fuse_d1.atr`,
the `h1` directory, and the FUSE-mounted `mnt/fuse_d2.img`. Stop the FUSE
process with Ctrl-C when finished; if needed, unmount with
`fusermount -u ./mnt` (some systems provide this command as `fusermount3`).

## Test harness

`run.sh` combines build, ATR cleanup, Atari800 launch, FUSE mount, and transfer
checks. By default, it connects to `smb://localhost/foo` and enables the
done-file watchdog. The watchdog waits for a `done` entry on `fuse_d1.atr` and
then stops the emulator and FUSE process. `-e` is accepted as an explicit
alias for the default. Use `-n` or `--noexit` to disable the watchdog and leave
the simulator running until manually stopped:

```sh
./run.sh smb://localhost/foo
./run.sh --headless
./run.sh --noexit
```

Use `-h` or `--headless` to run Atari800 under `xvfb-run -a`; `xvfb-run` must be
installed. `--help` prints all options. The optional positional URL overrides
the default share. `CDEFS` can be overridden as shown in the build section
above. The FUSE filesystem exposes the virtual disk as `fuse_d2.img`.

**Caution:** `run.sh` unmounts `./mnt`, removes test entries (`foo`, `bad`, and
`done`) from `fuse_d1.atr`, removes local `foo` and `bad` paths, and may kill
all processes named `atari800` or `dstitch_mount`. Review the script and make sure
the ATR and local test files are disposable before running it. For interactive
use, the manual two-terminal method above is safer. The script's `make clean`
removes the generated `dstitch_mount` executable before rebuilding it.
