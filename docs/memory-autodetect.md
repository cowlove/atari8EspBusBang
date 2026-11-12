# Native memory autodetection (design note, draft)

Goal: one firmware image that adapts to a 600XL, 800XL or 130XE (and
upgraded machines) by measuring the Atari at boot, instead of picking the
memory layout with a BOOT_CONFIG. No changes to core 1's bus loop: every
step here runs while a PBI command has the bus cold.

## What gets detected, and what it decides

| Measured | Decides |
|---|---|
| Native top of RAM: 16K, 48K or 64K | `baseMemSz`: how much base RAM the board must supply |
| Native extended banks: the PORTB bank values that reach distinct RAM at $4000 | `extMemConf` and `extMemSramBanks`: emulate RAMBO-style banks, or add banks on top of native XE ones |
| Whether the halt line works | `haltAvailable`, and with it whether PSRAM-backed banks (swaps need the 6502 halted) are possible |

A memory probe can't see the halt line (it's wiring: the XL's PBI has it,
the 130XE's ECI doesn't), so it's tested directly. That also tells an
upgraded 800XL (native banks, halt) from a 130XE (native banks, no halt).

Test-harness settings (disks, cartridge, boot keys, WiFi timing) stay in the
named configs; the hardware fields become automatic, with a manual override.

## Sequence

1. **Power-on: map all 64K of base RAM.** The OS sizes RAM before it calls
   the PBI init vector (AltirraOS `init.s`: measure into `tramsz`/`memtop`,
   clear, then `PBIScan`), so this is the only way a 600XL boots with 64K.
   Base RAM pages are write-through (`mmuAddBaseRam()`: extSel_ only on
   reads), so wherever native RAM exists it has received every write too.
2. **PBI init calls `PBICMD_PROBE_BEGIN`.** The ESP swaps in a probe map:
   board RAM for $0000-$3FFF (every machine has native RAM there anyway),
   the PBI ROM window, and nothing else; no cart, no bank emulation.
3. **The 6502 probes** (interrupts off: `SEI`, `NMIEN=0`, DMA off, all
   restored afterwards; on a 600XL ANTIC would otherwise fetch its display
   list from unmapped RAM):
   - RAM at $4000-$BFFF in 4 KB steps: save the byte, write two
     complementary patterns, read each back, restore. Patterns must not
     match what a floating-bus read returns (the last byte on the bus,
     often the operand's high byte).
   - Banks: every PORTB combination of bits 2-7 (64 values; OS ROM bit 0
     on, BASIC bit 1 off throughout). Write a distinct signature at $4000
     under each, then read every one back and report which value each
     reads back; the ESP sorts the results into the native scheme (which
     bits select banks, which alias main memory, CPU/ANTIC split). Self-test
     (bit 7 = 0) maps only $5000-$57FF, so $4000 is unaffected. Restore
     PORTB and the bytes.
   - Halt: the 6502 increments a counter in board-mapped page 0 in a tight
     loop; the ESP asserts halt_ for a known time and checks whether the
     counter stopped. (Needs: asserting halt_ is harmless where the line
     isn't connected, e.g. an XE on the ECI.)
4. **PBI init calls `PBICMD_PROBE_REPORT`** with the results. The ESP
   builds the final map and swaps it in (same `safeCopy()` path
   `disableBus()` uses). Regions handed back to native RAM need no copy:
   write-through kept native current, and the probe restored what it wrote.
5. **Once per power-on.** PBI init runs again on a warm reset; after the
   first report the ESP answers `PROBE_BEGIN` with "already done".
6. **Reload the overlay (deferred).** Not in the sketch: the overlay is
   loaded with `pbiROM[]` at boot only, and SIO copies overwrite `COPYBUF`
   afterwards. A reload has to happen before the resident `PBI_INIT`
   jumps to it (not at `PROBE_REPORT`, which SIO would undo), e.g. from a
   resident command ahead of the JSR, copying from a flash copy of the
   image. Deferred (user, 2026-11-12) until the larger bring-up and reset
   picture is worked out. The earlier note:
   **Reload the overlay on `PROBE_REPORT`.** SIO copies overwrite
   `COPYBUF` after boot, so the ESP copies the probe overlay back in when it
   handles the report, ready for the next time the Atari runs PBI init
   while the ESP stays up (a warm reset, or a reboot).

**Known gap: resets.** The board can't see the Atari's reset_ line yet, so
it can't tell a warm reset from an Atari power cycle while the ESP stays
up. An Atari-only power cycle after the probe would find the final map
(not all 64K) during the OS's RAM sizing, and a 600XL would size wrong.
Today the order of power-up is fixed by tools/boot.sh; synchronizing real
startups and resets with the board is a general TODO.

## Where the probe code lives

The PBI window is $D800-$DFFF, but $DC00-$DFFF is `COPYBUF` (the SIO copy
buffer), so code has $D800-$DBFF; the image is 750 bytes, about 270 free.
Proposal: the probe is an overlay. The ESP loads it into `COPYBUF` at boot
(nothing uses the buffer before PBI init) and `PBI_INIT` just `JSR`s to it.
The resident ROM grows by a few bytes; the probe gets a full 1 KB, and later
overlays (other init-time work) can reuse the trick.

## ESP side

- Base RAM allocated as separately freeable 16 KB blocks; after the report,
  free what native RAM covers and build the extended-bank pool from what's
  left, instead of at boot (`mmuInit()`, `ExtBankPool`).
- The probe must finish before WiFi starts (the PBI boot starts WiFi), or
  its allocations fragment the heap; the 600XL is already tight (cf24ba7c).
- Per-class data the configs carry today (`haltAvailable`, `extMemConf`,
  `extMemSramBanks`, `baseMemSz`) become the probe's output.

## Testing

- csim: atari800 can run as a 600XL, 800XL or 130XE, and `BUS_CYCLE=1` runs
  the board's MMU, so the probe and the remap can be tested on all three.
- csim halt (new): an approximate halt in atari800, enough for the
  detection test (does the 6502 stop making progress while halt_ is
  asserted). It doesn't make csim's halt good enough for PSRAM swaps:
  `mmuOnChange()` still runs a cycle tick late (see the csim-no-halt note).
- Hardware: .3/.4 (600XL), .1/.2 (800XL), .5/.6 (130XE).

## Answered

- The halt_ pin isn't connected to anything on the XE (ECI), so asserting
  it there is harmless: the halt test just sees the 6502 keep running.
- The real OS sizes RAM among its first steps, before PBI init: it has to
  know the top of RAM to place screen memory and start ANTIC.

## Answered (2)

- Bank-probe scope: every PORTB combination, results sorted out after
  (user, 2026-11-12): covers the 130XE and the 192K/256K/320K-style
  upgrades without knowing their schemes in advance.

## Halt test (built 2026-11-12)

In `handlePbiRequest()` for `PBICMD_PROBE_BEGIN`, right after the
stack-program wait (the 6502 is in the stack-resident loop, the bus is
attached, core 0's interrupts are masked) and before `req` is cleared:
page 1's read ctrl gets the halt_ bit and `pinReleaseMask` drops it, so
core 1 asserts and holds halt_ on the loop's next instruction fetch; three
samples of `DUMMY_WRITE` ~100 6502 cycles apart; all equal means the line
works; then the bit is cleared and the 6502 released (held ~300 cycles).
The stack loop's `sta DUMMY_WRITE` is now `inc DUMMY_WRITE` (same 3 bytes),
so the byte changes every pass; no other 6502 code. Reported with the
probe results.

csim: core 0 runs as a coroutine in bus-cycle mode (it runs the real
`core0Loop()` and yields to atari800 while it waits), so the check runs
unchanged there: `DUMMY_WRITE c2 c2 c2` (present) with the halt line,
`c6 cf d8` (absent) with `CSIM_HALT_LINE=0`.

Replaced: the first sketch (42de1119), a PBI_INIT counter loop with
`PBICMD_HALT_TEST` and a state machine on core 0's utility tick (~100 ms
hold, an 8-bit count that could match by wraparound).

## Memory probe: sketch status (2026-11-12)

Built: the overlay at $DC00 (`pbirom.asm`, 248 bytes; the resident
`PBI_INIT` just `JSR`s to it after the halt test), `PBICMD_PROBE_BEGIN`
(probe map into `mmuStateSaved`: $4000-$BFFF, `extBanks[]`,
`basicEnL1BlockMux[]`, `cartBanks[]` to the dummy block; the real map kept
in a temporary heap copy) and `PBICMD_PROBE_REPORT` (print, restore).
`mmuOnChange()` leaves banks alone while the probe runs. Results are only
printed; nothing is reconfigured yet.

csim BUS_CYCLE results:
- 800XL (`-xl`): halt line present; RAM to 48K; all 64 PORTB values share
  one RAM at $4000 (no banks). Boot unchanged afterwards.
- 130XE (`-xe`, `CSIM_HALT_LINE=0`): halt line absent; RAM to 48K; 5 RAMs
  at $4000: main plus 4 banks on PORTB bits 2-3 with bit 4 low; the 32
  values with bit 4 set share main memory.
- 16K XL (atari800 config `RAM_SIZE=16`): halt line present; RAM to 16K.
  The boot then crashes (CIM at $C005), identically without the probe, so
  it's csim's 16K-XL setup, not the probe. Pinned (user): not worth
  chasing 600XL emulation in atari800 for now.

## Base RAM: use-tracking allocator (decided 2026-11-12)

AUTO maps 64K at power-on and shrinks to `minBaseRam` after the probe on
machines with 48K+ native. Rather than re-running `mmuInit()`:
`mmuAllocAddBaseRam()` allocates in pieces of at most 4K (IDF's heap is
good-fit, so they fill holes first; built 2026-11-12), so base RAM fills small heap regions and holes and leaves
the big contiguous blocks for 16K bank buffers and WiFi, and records which
allocation owns each page; `mmuFreeUnmapBaseRam(start, end)` unmaps those
pages in every live L1Block that points at them (a registry of L1Blocks:
`l1Blocks[]`, the `osRom*`/`basic*` copies, cartridge and extended-bank
blocks) and frees each allocation when its last page goes. Special regions
(PBI ROM, I/O pages, PBI RAM, the screen mapping) are untouched. Contiguous
pieces keep SIO's direct path (`mmuCheckRangeMapped()`); a sector spanning
two pieces falls back to the 6502 copy path, correct but slower.

## Bookmarked
- Resumable scatter/gather for SIO (user, 2026-11-12): copy sector data
  into/out of disjoint ESP pieces, return NEEDS_COPYIN / RES_COPYOUT for the
  parts the 6502 must copy, and resume where it left off when the command
  is re-issued. A general SIO speedup too; deferred in favour of the
  use-tracking allocator.

- More PBI ROM space if it's ever needed: `PBI_INIT`'s HATABS insert is
  written out three times; as a loop it saves ~40 bytes.
