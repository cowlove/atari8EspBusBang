# SDFS stitched disk design

## Goal

Add a native SpartaDOS File System (SDFS) virtual disk, implemented as
`DiskStitchImageSDFS`, alongside the current DOS 2 stitched image. It should
present files from a backing store (initially the existing file-oriented
storage adapters such as SMB) as a disk image that SDX mounts as SDFS.

The motivation is to get beyond the current DOS 2 image's small, fixed
geometry and flat directory limits so that more and larger backing files can
be exposed in one virtual disk. “Almost arbitrary” is relative to DOS 2, not
literally unlimited: SDFS sector-number, volume-size, and file-length fields
are finite. The exact supported ceiling must be established from the target
SDX version and formatter before choosing image geometry.

Subdirectories are a possible later extension, not a 1.0 requirement. The
initial design targets a flat root directory.

## Terminology and compatibility

SpartaDOS X (SDX) is the operating system; SDFS is its native disk filesystem.
SDX can also access DOS 2 disks through its DOS 2 driver, and the current DOS 2
stitched image is already usable from SDX. This project is specifically about
adding a native SDFS stitched image, not merely improving DOS 2 compatibility.

## Existing architecture

- `DiskImage` is the sector-device boundary used by the firmware (`main/diskImage.h`).
  It exposes sector reads/writes and image geometry.
- `StorageInterface` is the file-oriented backing-store boundary
  (`main/diskStitch.h`). SMB and ProcFS are storage implementations, not disk
  filesystem formats.
- `DiskStitchImage` builds a DOS 2 image: its VTOC/bitmap, 16-byte directory
  entries, sector allocation, and chain encoding are DOS 2-specific.
- The FUSE simulator consumes a `DiskImage`, so it should be possible to
  select a second image implementation without changing the FUSE protocol.

The SDFS implementation should therefore be a separate `DiskImage` class,
`DiskStitchImageSDFS`. It can reuse the backing-store interface and the sector
mounting path, but should not inherit DOS 2's directory, VTOC, or allocation
rules. Extract lower-level helpers only when both implementations demonstrate
a genuinely shared operation.

## Development and test environment

`csim/dstitch` is the primary development environment for this work. It can
build the FUSE mount and attach the virtual disk to Atari800, allowing quick
iteration against SDX behavior without first building/flashing the firmware.
Use that simulator loop for format bring-up and behavioral tests; reserve
firmware builds and hardware checks for later integration validation. SDFS
tests should use a known-good SDX-formatted image and should be kept separate
from the existing DOS 2 `run.sh` workload.

## SDFS format notes

The SDX 4.50 User Guide describes SDFS in §7.1 (including §7.1.3, “Sector
Maps”) and §7.2 (“Directory Structure”), printed pages 171–175. The important
structural differences from DOS 2 are:

- The volume's first sector identifies the root directory through a sector-map
  pointer. Volume metadata and allocation information also live in the SDFS
  layout; implementation field values still need validation against a
  formatter-produced image.
- Files and directories are represented through sector maps. A map sector has
  links to adjacent map sectors and lists the data-sector numbers for that
  file/directory. Map sectors themselves can form a chain.
- A directory is a file containing 23-byte records; it is not the DOS 2 array
  of 16-byte directory entries. The first record is directory metadata, with
  subsequent records describing entries.
- SDFS sector addresses and file lengths have finite widths. The guide confirms
  16-bit sector numbers and 3-byte lengths, so the volume sector-count field
  caps the geometry at 65,535 sectors. `DiskStitchImageSDFS` defaults to this
  maximum; `--sdfs-sectors N` in `dstitch_mount` allows smaller geometries for
  tests and deployment choices. It cannot exceed the SDFS address limit.

The guide specifies little-endian 16-bit sector pointers/counts, 3-byte file
lengths, and these SDFS 2.1 structures: sector 1 has the root-map pointer at
offset 9, volume sector count at 11, free count at 13, bitmap-sector count at
15, and bitmap start at 16. The bitmap is contiguous; bit 7 represents the
first sector in each group of eight, with set bits meaning free. A sector map
stores next/previous map-sector numbers at offsets 0/2 and 16-bit data-sector
numbers from offset 4. At 256 bytes per sector this leaves 126 data-sector
slots per map. Directory entries are 23 bytes; the root's first record is a
directory header, followed by file records. SDFS 2.1 uses a 16-bit sector
count, so 65,535 sectors is the current prototype bound, not unbounded
capacity. For 128/256-byte media, only 128 bytes are used in each of the first
three boot sectors; SDX does not require this data volume to be bootable.

Use the SDFS chapter as the format authority rather than inferring behavior
from the SDX version number. Where the manual is ambiguous, compare with an
image formatted by SDX 4.50 and with SDX's own directory/file operations.

## Proposed 1.0 scope

- Present a flat set of regular backing files in the SDFS root directory.
- Synthesize valid volume metadata, root-directory records, sector maps, and
  allocation state for the selected image geometry.
- Map SDFS data-sector reads and writes to the corresponding backing file and
  byte offset. Keep host file length and SDFS directory length consistent on
  writes, truncation, and overwrite.
- Do not expose nested backing directories, symlinks, or host-specific paths
  inside SDFS in 1.0.
- Define name conversion and collision behavior explicitly. SDFS names and the
  host namespace may not have identical limits or case rules.

Whether 1.0 permits creating/deleting files, or only reading and writing the
initial inventory, remains an open product decision. If creation is supported,
the storage adapter's open/create result should determine whether it succeeds;
the SDFS layer must report the corresponding disk error and avoid publishing a
directory record for a failed backing-file creation.

## Phase 1: read-only flat SDFS image

**Objective:** make SDX 4.50 mount a synthesized SDFS volume, list a flat set
of backing files, and read their contents accurately. Do not implement file
creation, deletion, or writes in this phase.

### Work

1. **Lock down the on-disk facts.** Use the SDFS chapter of the SDX 4.50 User
   Guide (§§7.1–7.2) and inspect one or more volumes formatted by SDX 4.50.
   Record the sector size/count, reserved sectors, volume/allocation metadata,
   root-directory map pointer, sector-map link fields, directory-header
   record, and file-record fields. Resolve field widths, byte order, name
   rules, and maximum supported geometry before encoding them.
2. **Choose a bounded geometry.** Select one supported sector size and a
   conservative sector count below the confirmed SDX/SDFS limit. Reserve the
   boot/volume, allocation, root-directory, and map sectors explicitly. Do
   not claim dynamic or near-unbounded capacity yet.
3. **Inventory a flat backing directory.** At image start, enumerate regular
   files from the selected `StorageInterface` path and make a stable snapshot
   for the mount lifetime. Preserve representable unique names, and assign
   deterministic short aliases to names that need abbreviation. Reserve all
   representable true names before generating aliases; if an alias candidate
   collides with a true or already-assigned name, skip that number and try the
   next one. Use only one decimal suffix digit (`1`–`9`); if a collision group
   exhausts those candidates, omit its remaining files from the Atari
   directory. Report omissions in host-side diagnostics. Files added after
   startup remain invisible until the image is reconstructed, so no live
   alias updates or persistent alias database are required. Recursive
   enumeration is out of scope.
4. **Synthesize the volume.** Implement `DiskStitchImageSDFS : DiskImage` to
   construct valid volume metadata, allocation state, root directory records,
   sector maps, and file-data mappings. Keep this implementation separate
   from the DOS 2 `DiskStitchImage` rules.
5. **Keep the image read-only.** Reads map virtual SDFS data sectors to the
   snapshotted backing files. Writes return the proper write-protected result
   and must not mutate the virtual directory or mapping state.
6. **Expose it in the simulator.** Add an explicit resource selection in
   `csim/dstitch/dstitch_mount` and exercise it under Atari800 with SDX 4.50
   loaded from its normal boot device. The SDFS image is a mounted data disk;
   making it bootable is not a Phase 1 requirement.

### Implementation status

The first isolated prototype is in `main/diskStitchImageSDFS.h` as
`DiskStitchImageSDFS : DiskImage`. It currently synthesizes a 256-byte-sector,
65,535-sector SDFS 2.1 data volume with a flat root directory, 23-byte records,
linked sector maps, a free-space bitmap, and sector mappings to backing
files. It snapshots the backing directory at construction and sorts the
encoded 8.3 names for repeatable allocation. This geometry has a finite capacity
of about 16 MiB, not arbitrary size; 512-byte SDFS 2.1 sectors and other
geometries are not implemented.

Both DOS 2.0 and SDFS now use the shared deterministic 8.3 name mapper in
`main/diskStitch.h`. It reserves representable true names first, then assigns
aliases to names that cannot be represented directly. Generated aliases use
at most seven sanitized base characters plus one digit (`1`–`9`); the
extension is limited to three characters. A generated alias must not shadow a
true name or an earlier alias, so occupied suffix numbers are skipped. Files
left without an available suffix are omitted from the Atari directory and
logged. The mapping lasts for the image instance; aliases may change after a
restart if the backing inventory changes. This is intentionally simple and
does not persist an alias database.

### Writable step: existing-file in-place updates

The current writable step accepts writes to data sectors belonging to files
present in the initial inventory. It writes only the bytes within the
snapshotted file length, so a final partial sector does not pad or grow the
host file. Identical writes to synthesized metadata are accepted as no-ops;
metadata changes requiring allocation or a changed file length return
write-protected. File creation, deletion, growth, and truncation are not yet
supported. This gives us safe content updates without claiming full writable
SDFS allocation semantics.

The backing store is authoritative; synthesized SDFS metadata is ephemeral and
is rebuilt on each mount. We therefore do not need a persistent journal or
crash-recovery format for this application. During a live mount, however, each
operation should keep its in-memory directory, sector-map, and allocation
views coherent, and report backing-store failures promptly. For the common
single-backing-operation case, first prepare and validate the virtual metadata
changes in temporary state, including checking that any needed data/map
sectors are available. Then perform the backing-store operation. On failure,
discard the temporary plan and return the error without changing the published
in-memory metadata. On success, apply the already-validated plan to the
in-memory directory, maps, and allocation state; those virtual updates have no
further I/O failure point. This avoids a persistent transaction/recovery
mechanism while keeping the live image coherent. A later remount reconstructs
the virtual metadata from the then-current backing files.

The simulator CLI separates backing resource from image format. Use
`file://<local-directory>` for a local directory and `--sdfs` to select this
image; `--dos20` selects DOS 2.0 and is the default. SMB URLs remain a separate
backend selection. This selector is only in
`csim/dstitch/dstitch_mount.cpp` for rapid Atari800 testing. Firmware
`main.cpp` remains unchanged. The class compiles through that simulator target,
and a sector-level smoke test now checks sector 1, allocation bits, root
directory records, file bytes, empty files, and a 127-sector file crossing the
126-entry map boundary. A temporary FUSE mount exposed the expected 16,776,576
byte image and returned the synthesized sector data. Atari800/SDX mount/list
behavior has not yet been validated against SDX 4.50; sector-1 fields and
generated metadata still need comparison with a formatter-produced reference
volume.

The existing `csim/dstitch/autoexec.bat` is not a Phase 1 acceptance test: it
copies and overwrites files on D2. Use a dedicated SDFS test workload instead.

For write-order investigation, `dstitch_mount --sdfs-trace-overlay` enables a
temporary RAM overlay. Every SDFS sector write is accepted into memory, logged
with a monotonically increasing sequence number, sector number, initial
classification, changed-byte count, and before/after bytes for changed rows.
Subsequent reads return the overlay sector in preference to synthesized
metadata or backing-file data. The option never writes to the backing store and
is intentionally a tracing aid, not product write support; all overlay state
is discarded when the mount exits. It is useful for observing SDX's write
sequence for file creation and extension without the first rejected metadata
write stopping the operation. When exactly one in-progress create dirent is
visible, new-sector writes are tagged with that file's name; overlapping
in-progress file dirents produce a trace-check failure. The trace also compares
the order in which new sectors were first written with the data-sector order
later recorded in that file's first map sector. This explicitly checks the
append-order assumption needed to attribute not-yet-mapped sectors. It is
diagnostic evidence for the exercised SDX sequence, not a permanent filesystem
guarantee; the current order check is limited to one map sector. These checks
are permanent correctness requirements for writable SDFS, not disposable
tracing instrumentation: the normal write path must preserve file ownership and
verify sector ordering before it commits inferred data-sector mappings. If
either invariant is violated, it must report an error rather than silently
assigning data to the wrong backing file. The trace-mode checks are the current
prototype for validating those invariants against SDX behavior.

The current experimental write POC is opt-in with `--sdfs-write-poc` (also
forwarded by `run.sh`). It stages a new file's sector writes in RAM and writes
the backing file only after the guest has produced a final dirent, a sector-1 volume
update, and a map matching the observed new-sector write order. It checks the
bitmap allocation bits, 24-bit length, directory entry, and one-map capacity
before creating the host file. After backing data succeeds, it commits the
validated guest bitmap, directory, and map sectors into the stitched in-memory
metadata and drops that transaction's RAM overlay and hypothesis list.
Backing-file creation/write failures are returned to SDX; a partial host file
is removed best-effort. This POC handles
new 8.3 files with at most 126 data sectors in one map. It deliberately does
not support extending inventoried files, multiple map sectors, deletion, or
truncation/resizing. Keep the default mode write-protected and use a disposable
backing directory for POC testing.

### Acceptance criteria

- SDX 4.50 mounts the virtual disk as SDFS and lists every supported file once
  with the expected name and exact length.
- Read-back bytes match the source for empty files, short files, files crossing
  a data-sector boundary, and files crossing a sector-map boundary.
- Volume free-space and geometry reports are internally consistent with the
  generated allocation map and reserved metadata sectors.
- An attempted write is rejected as write-protected; subsequent listings and
  reads remain unchanged.
- The same generated image can be mounted again and produces the same layout
  for the same input snapshot.

### Explicitly deferred

Persistent writable files, create/delete, resize/overwrite, sparse files,
recursive host directories, dynamic changes to the backing directory while mounted,
boot-from-this-volume support, and geometries beyond the initial verified
bound are later phases. These should not be smuggled into Phase 1 merely to
make the first image appear more general.

## Suggested implementation boundaries

Keep format policy inside `DiskStitchImageSDFS` (or small SDFS-specific helper
types):

- SDFS volume/boot metadata and geometry
- root directory and 23-byte record encoding
- sector-map traversal and construction
- allocation/free-space bookkeeping
- SDFS error/status interpretation for directory and data writes

Reuse existing interfaces where they fit:

- `DiskImage` for sector access from the firmware and simulator
- `StorageInterface` for listing/opening/reading/writing/truncating backing
  files
- the existing FUSE mount layer and `dstitch_mount` resource selection, with a
  new resource selector for the SDFS implementation

Do not add format conditionals throughout `DiskStitchImage`. A shared
filesystem-neutral layer should be considered later only if the SDFS
implementation reveals duplicated, format-independent behavior (for example,
backing-file caching or sector-to-file byte I/O).

## Design questions to resolve

1. Validate the sector-1 metadata fields and full 65,535-sector geometry
   against SDX 4.50 and a formatter-produced image.
2. Should Phase 1 remain at 256-byte sectors, or add SDFS 2.1's 512-byte mode?
3. Which SDFS-legal characters should be preserved in direct names and in
   generated aliases? The alias allocation policy is agreed: one-digit
   suffixes `1`–`9`, skip occupied candidates, and omit files after exhaustion.
4. Should a later writable phase expose a stable snapshot, or reflect backing
   directory changes while mounted?
5. Which mutations and storage failures should a later writable phase support?
   Recursive path enumeration remains deferred with subdirectories.

## Validation plan

1. Create or obtain a blank SDFS image formatted by SDX 4.50; preserve a copy
   as a known-good reference and record its sector geometry.
2. Document sector 1, the root map chain, allocation metadata, and directory
   records from that image and the manual before writing an encoder.
3. Implement a read-only flat image first. Compare generated sectors with the
   reference layout and verify SDX can mount it, list the root, and read files
   with boundary sizes (empty, one sector, map-boundary, and maximum-size
   cases).
4. Add writes and mutation support only after the read-only layout is accepted.
   Test sector-spanning writes, EOF sizes, shorter/longer overwrite, map-chain
   growth, allocation exhaustion, create failure, and remount/re-read behavior.
5. Keep DOS 2 regression tests separate; do not treat the DOS 2 `run.sh`
   harness as a substitute for SDFS format validation.

## References

- [SpartaDOS X 4.50 User Guide](https://sdx.atari8.info/sdx_files/4.51/SDX450_User_Guide.pdf),
  especially §§3.12, 6.3, 7.1–7.2, and 8.6.1. The SDFS format discussion is in
  the printed page range 171–175.
- [Official SDX documentation index](https://sdx.atari8.info/index.php?show=en_docs)
