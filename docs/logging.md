# LOG(), LOGHOT() and the Sidecar console

How the host app logs, and how its log reaches the Sidecar's console
(2026-11-14, exp-sidecar-ota).

## The macros (components/a8log/include/a8log.h)

| Macro | What it is | Where it may run |
|---|---|---|
| `LOG(fmt, ...)` | `printf()`'s drop-in: same arguments, same return value | cold code, any task |
| `LOGL(level, fmt, ...)` | `LOG()` with a level | cold code, any task |
| `LOGHOT(fmt, ...)`, `LOGHOTL()` | formats where `printf()` can't | core 0 with core 1 hot (interrupts masked) |

Every `printf(` in `main/` and the shared components (disk_image,
sidecar_link, bus_profiler) is now `LOG(` (312 calls). The Sidecar app's
own sources and csim's keep `printf()`. diskStitch's silent `LOG()` is
now `STITCH_LOG()`.

**Levels: a sketch so far.** The levels are A8LOG_ERROR 1, WARN 2, INFO 3,
DEBUG 4 and VERBOSE 5, numbered as in esp_log.h.
- `A8LOG_MIN_LEVEL` drops calls at compile time; `a8logLevel` (default
  INFO) drops them at run time, in Sidecar-route builds only.
- Every existing call is INFO. They haven't been sorted into levels.
- Each SerialOut frame carries the most severe level in it, for the
  Sidecar to use later (colour, filtering).

## Routes

- **Without the Sidecar link** (the test pool, the Sidecar app, csim's
  console default), `LOG()` is `printf()`.
- **With it** (`CONFIG_A8BB_SIDECAR_ENABLE`), `CONFIG_A8BB_LOG_VIA_SIDECAR`
  defaults to y and `LOG()` becomes `a8log_printf()` (main/a8log.cpp):
  - **Console tee:** with `CONFIG_A8BB_LOG_CONSOLE_TOO` (default y) it
    still prints here. boot.sh's DONE and `io:` detection, crash bundles
    and the pool all read this console, so the tee stays on until they
    read the Sidecar's.
  - **The ring:** `SidecarHost::log()` `vsnprintf()`s the line into a
    4 KB RAM ring. Any task may append; the append is a short masked
    section.
  - **Who sends:** only the link's owner, the setup()/core 0 task
    (`SidecarHost::logDrain()` → `Sidecar::serialOut()`). The link is
    single-threaded, and another task's `LOG()` can run in an
    interrupts-enabled window while core 0 sits in a transact.
- **When it sends:**
  - right away, from a cold `LOG()` on the owner task with interrupts on,
    at a line end or once a frame's worth (281 bytes) is waiting;
  - one frame per PBI request while the bus is hot, before `req = 0`, on
    both the Sidecar and local paths. The 6502 is parked then anyway.
  - **never** from the bmon loop's utility pass, for the reason the
    Sidecar poll is off there on master.
- **Fallbacks:** plain `printf()` until the Sidecar answers its boot
  report, with a Sidecar too old for SerialOut, and in diagnostic mode.
  Three unanswered frames in a row turn the route off for good (console
  only, reported on the next cold `LOG()`). A "dropped" answer (the
  Sidecar's buffer is full) just retries.
- **csim:** `CSIM_LOG_ROUTE=console` (default) | `sidecar` | `both`.
  csim's Sidecar prints to the same stdout.

## Protocol (components/sidecar_link/sidecar.h)

- **`SerialOut` (command 8, main → Sidecar):** `SidecarSerialOutBody`
  {level, length} plus the text, which may be any split of the output.
  The Sidecar copies it into an 8 KB ring and prints it after
  `service()` returns, so a blocked console never holds up the response.
  Status 0 means taken, 1 dropped, 2 bad length.
- **BootStatus's reserved byte** is now `capabilities` in the reply; the
  Sidecar sets `SidecarCapability::SerialOut`. An unknown command gets no
  response, so the sender would wait out its timeout. An older Sidecar
  echoes 0 here, so main never sends it SerialOut.

## Costs, measured

Measured on the a24 pair (main E8:F6:0A:D2:EE:78 + Sidecar FC:01:2C:2D:8A:3C),
FAKE_SIO + FAKE_CLOCK, 120 s runs:

| | |
|---|---|
| SerialOut round trip | mean 2.0-2.3 ms per frame, max 2.5 ms (one run 6.3 ms) |
| frames per 120 s run (console tee on, FAKE_SIO's report lines) | ~240, 0 failed |
| added to a PBI request with text waiting | one frame: ~2.2 ms |
| what arrived | the run's `io:` lines, identical on both consoles |

The 2.2 ms is the price a PBI request pays when log text is waiting: the
6502 sits in the stack program meanwhile. Requests with nothing queued
pay nothing (one compare).

**Internal RAM.** On the S3, IRAM code comes out of the same SRAM as the
heap: the 16 KB IRAM region is full, and the rest is DIRAM.

| | IRAM (code) | DRAM |
|---|---|---|
| ring, drain path (a8logRing*, logDrain, serialOut, a8logDrainHot) | ~410 B | 4 KB ring + 2 × 300 B message buffers |
| LOGHOT entry + append (a8log_hot, a8logRingPut) | ~200 B | 160 B of stack per call |
| LOGHOT's formatter (nanoprintf, this config) | ~1640 B | each LOGHOT format string (DRAM_STR) |
| Sidecar side | n/a | 8 KB ring (the Sidecar has room) |

## Question 2: LOGHOT, hot-bus safe? Worth it?

**Hot-safe:** yes, as built.
- `vsnprintf()` is out: newlib code in flash, and possible locks.
- The formatter is nanoprintf (single header, 0BSD/Unlicense,
  fuzz-tested against libc), compiled into internal RAM: width,
  precision, hh/h/l/ll/z/j, `#`, no floats.
- The post-link hot-call check roots at `a8log_hot`, `a8logRingPut` and
  `a8logDrainHot`: 31 functions, no flash code or data, no peripherals.
  Its only ROM calls are 64-bit shifts.
- The rules the check can't see:
  - the format must be a literal (the macro puts it in DRAM);
  - `%s` arguments must point to RAM;
  - no floats.

**Alternatives measured:**

| formatter | IRAM (-Os) | notes |
|---|---|---|
| nanoprintf, the config used | 1771 B | width, precision, hh/h/l/ll/z/j, `#` |
| nanoprintf, minimal | 1013 B | width only, no ll/z/hh, no `#` |
| a formatter of my own (342/342 against libc) | 2077 B | dropped: bigger and less proven |
| deferred formatting (store fmt pointer + argument words, format later) | ~150-200 B | not built (below) |

**Deferred formatting** costs almost nothing hot, but the text needs a
`vsnprintf()` somewhere:
- **On main:** only at cold points. In a run whose disks are all the
  Sidecar's, those barely exist, so LOGHOT lines would surface late.
- **On the Sidecar:** it would need the format string. Sending it means
  copying it hot, so it must be in RAM anyway. It also needs protocol
  work and copies of `%s` strings.

It would win if internal RAM gets tight. Most of the plumbing is shared
either way, so switching later is cheap.

**Is it worth it?** For the Sidecar build: yes, I think.
- About 1.8 KB of internal RAM buys text logging from code that today
  can only fill StructLog rings, which are typed, binary, and printed at
  exit.
- The first use already fills a gap: a lost Sidecar SIO request logged
  nothing until now.
- Where every KB of SRAM counts (bank-heavy configs; the 600XL's WiFi
  failed at 17.8 KB free), drop to nanoprintf's minimal config (−760 B),
  or gate it with a Kconfig option. Each call site also costs its string
  in DRAM.

**What LOGHOT doesn't do:**
- It doesn't print on main's console (nothing hot may), only the
  Sidecar's.
- It does nothing in builds without the Sidecar route. It could: build
  the ring everywhere and flush it with `printf()` at the cold points
  (handleSerialIO's window, the end of a run), as FAKE_SIO's message
  queue does.

## Open

- The Sidecar prints main's text raw, with no prefix. Its own logs
  interleave line by line, but a frame boundary can fall mid-line.
- Tools (boot.sh's DONE detection, crash bundles) still read main's
  console. Switching them to the Sidecar's console is what would let
  `CONFIG_A8BB_LOG_CONSOLE_TOO` go off.
- IDF's own `ESP_LOGx` output (and panic output) doesn't go through
  `LOG()`.
- Piggyback, not built: an SIO read's command frame has ~279 unused data
  bytes, which could carry log text for free instead of a separate
  SerialOut round trip.
