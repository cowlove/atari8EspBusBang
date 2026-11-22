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
- **With it** (CMake `-DSIDECAR=ON`, as `boot.sh -sc` builds;
  `A8BB_SIDECAR`), `A8BB_LOG_VIA_SIDECAR` is 1 (0 in a
  `-DSIDECAR_LOOPBACK=ON` build) and `LOG()` becomes `a8log_printf()` (main/a8log.cpp):
  - **Console tee:** with `A8BB_LOG_CONSOLE_TOO` (1 unless
    `-DSIDECAR_LOG_ONLY=ON`) it
    still prints here. boot.sh's DONE and `io:` detection, crash bundles
    and the pool all read this console, so the tee stays on until they
    read the Sidecar's.
  - **Synchronous send** (the user's call, 2026-11-14):
    `SidecarHost::log()` formats with `vsnprintf()`; LOGHOT's
    `SidecarHost::logHot()` formats with nanoprintf. Either sends from
    the same call (`logPut()` → `Sidecar::serialOut()`), line-buffered as
    stdout is: a frame at each newline, or when the 281-byte frame fills.
    A StructLog hex dump's `"%02x "` pieces would otherwise cost a round
    trip each.
  - **Who sends:** only core 0's task, which has no concurrent loggers.
    `LOG()` from any other task only prints here.
  - **Not re-entrant:** an `inProgress` flag, and no sending inside any
    other link call (`Sidecar::busy()`). A `LOG()`/`LOGHOT()` from in
    there (SidecarHost and below) isn't sent; it's counted. Future work: a
    short SRAM trace of such lines, or of the most severe levels (a
    CRITICAL).
- **Fallbacks:** plain `printf()` until the Sidecar answers its boot
  report, with a Sidecar too old for SerialOut, and in diagnostic mode.
  A frame the link doesn't take is lost. Three unanswered in a row turn
  the route off for good (console only, reported on the next cold
  `LOG()`). A "dropped" answer (the Sidecar's buffer full) loses only
  that frame.
- **csim:** `CSIM_LOG_ROUTE=console` (default) | `sidecar` | `both`.
  csim's Sidecar prints to the same stdout.

(Before that change, 8375c06..042ed1b queued text in a 4 KB ring and sent
it later from fixed drain points: cold `LOG()`s on the owner task, and
one frame per PBI request while hot.)

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
| each line sent (LOG at a newline, or a LOGHOT) | one frame: ~2.2 ms, there and then |
| what arrived | the run's `io:` lines, identical on both consoles |

Each line costs its ~2.2 ms where it's written: in cold code (boot, exit
reports, the interrupts-enabled windows, where the 6502 sits in its stack
program) and, for LOGHOT, in the hot code itself.

**Internal RAM.** On the S3, IRAM code comes out of the same SRAM as the
heap: the 16 KB IRAM region is full, and the rest is DIRAM.

| | IRAM (code) | DRAM |
|---|---|---|
| send path (logPut, logSendLine, logFlush, serialOut), LOGHOT entry (a8log_hot, logHot) | ~600 B | 281 B line buffer + 2 × 300 B message buffers; 160 B of stack per LOGHOT |
| LOGHOT's formatter (nanoprintf, this config) | ~1640 B | each LOGHOT format string (DRAM_STR) |
| Sidecar side | n/a | 8 KB ring (the Sidecar has room) |

## Question 2: LOGHOT, hot-bus safe? Worth it?

**Hot-safe:** yes, as built.
- `vsnprintf()` is out: newlib code in flash, and possible locks.
- The formatter is nanoprintf (single header, 0BSD/Unlicense,
  fuzz-tested against libc), compiled into internal RAM: width,
  precision, hh/h/l/ll/z/j, `#`, no floats.
- The post-link hot-call check roots at `a8log_hot` and
  `SidecarHost::logFlush`: 29 functions, no flash code or data, no
  peripherals.
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
- It does nothing in builds without the Sidecar route, and nothing when
  called from inside the send path (SidecarHost and below). Both could
  feed a short SRAM trace, printed at cold points (handleSerialIO's
  window, the end of a run) as FAKE_SIO's message queue is: future work,
  along with a CRITICAL level kept there.

## Open

- The Sidecar prints main's text raw, with no prefix. Its own logs
  interleave line by line, but a frame boundary can fall mid-line.
- Tools (boot.sh's DONE detection, crash bundles) still read main's
  console. Switching them to the Sidecar's console is what would let
  `A8BB_LOG_CONSOLE_TOO` go off.
- IDF's own `ESP_LOGx` output (and panic output) doesn't go through
  `LOG()`.
- Piggyback, not built: an SIO read's command frame has ~279 unused data
  bytes, which could carry log text for free instead of a separate
  SerialOut round trip.
