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
6. **Reload the overlay on `PROBE_REPORT`.** SIO copies overwrite
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

## Bookmarked

- More PBI ROM space if it's ever needed: `PBI_INIT`'s HATABS insert is
  written out three times; as a loop it saves ~40 bytes.
