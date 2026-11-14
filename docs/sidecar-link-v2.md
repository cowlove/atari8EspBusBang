# Sidecar link v2: symmetric frames and object fetch

Status: **built, bench-verified** (2026-11-13). Designed with the user;
implemented in `components/sidecar_link/sidecar.h` / `sidecar.cpp` and
`sidecar_fetch.inc`, replacing v1. Verified in csim and on the bench pair
(no Atari): fetches both ways, Test and Firmware objects, diagnostic mode.

## Why

v1 is one-way: the main board sends commands, the Sidecar only answers.
Updating the main app over the link (OTA) has main fetching from the
Sidecar, which v1 can do, but other planned features run the other way (the
Sidecar fetching main's core dump, logs). v2 lets either side ask, with one
frame format, without making the main board's code any more complicated.

## The link underneath (unchanged)

i2slink carries one fixed payload per direction (`I2SLINK_PAYLOAD_BYTES`,
304), resent every wire cycle (585 us at 200k frames/s). Each direction is a
"latest value", not a queue: a new write replaces the old one on the wire,
and the reader sees each version at most once.

## Frames

The same layout in both directions:

| Field | Bytes | |
|---|---|---|
| `version` | 1 | protocol version, 2 (a mismatched peer is reported, not served) |
| `mode` | 1 | `Idle`, `Command` or `Response` |
| `frameSeq` | 2 | this side's frame sequence: bumped on every new frame |
| `ackSeq` | 2 | the last `frameSeq` this side has seen from the peer |
| message header | 7 | as v1: type, sequence (a command's; a response echoes it), flags, status |
| body + data | 291 | the command's body (DCB, IOCB, ...) then data; a 256-byte sector fits |

A frame is either a command or a response, never both: `mode` says which,
and that one owns the data area. (No split data area: too small for a
sector.) Since only one is ever live, one message header serves both.

## Rules

1. **Ack before any new frame.** A side may write a new transmit frame, of
   either mode, only when the peer's `ackSeq` equals the `frameSeq` it is
   currently sending: the peer has seen it. Nothing is ever overwritten
   unseen. In practice the peer's next frame, written after reading ours,
   already carries the ack, so the wait is usually zero. **Acking is always
   allowed:** rewriting the current frame with only a new `ackSeq` (same
   `frameSeq`, same content) is not a new frame and needs no ack, else
   both sides could wait on each other forever. A side acks each new peer
   frame as soon as it reads it, so both sides poll the link: the main
   board from its core 0 loop (one memory-only `i2slink_hint()` when
   nothing's new), the Sidecar from its service loop.
2. **The receiver copies what it needs** out of a frame when it first sees
   it (the sender may replace it as soon as it's acked).
3. **One outstanding command per side.** A side waits for the response to
   its command before sending another.
4. **Priority: the main board.** Waiting for a response, the main board
   ignores incoming commands and keeps waiting. Its SIO/CIO commands are
   never interrupted, so its code stays issue, wait, done.
5. **The Sidecar yields.** Waiting for a response and seeing a command
   instead, the Sidecar abandons its own command (its call returns
   `Preempted`), serves the incoming one, and its caller retries later with
   a new command sequence. Rule 1 guarantees an abandoned command was never
   run: the main board can't replace a response with a new command until
   the Sidecar has seen the response.
6. **Idle sides serve.** A side not waiting on its own command answers
   incoming commands.
7. **Restarts.** As in v1: a side that restarts may find the peer's last
   command still on the wire and must not run it twice, so it drops the
   first command it sees after starting; a Nop probe (now from either side)
   re-establishes the link.

## Commands

Kept from v1, now in the shared frame: `Nop` (probe/presence), `Sio`, `Cio`,
`Mount`. Main to Sidecar for now; the frame doesn't care.

New, for fetching, in either direction:

| Command | Arguments | Response |
|---|---|---|
| `ReadVersion` | object | status, size, version token |
| `ReadChunk` | object, offset | status, data (length = the data length) |

And Sidecar to main: `SetDiagnostic {enable}` (response: the mode now and
before). Normally the main board serves the link every ~50 ms, from
`core0_utilityTasks()`, so a fetch from main gets one chunk per 50 ms.

**Diagnostic mode** is where anything touching main's flash happens: core
dump fetches and firmware updates (decided 2026-11-13). Entering it, main
detaches the bus for good (as a PBI command's detach does, but never
reattached), re-enables interrupts and serves the link in a tight loop;
bmon, PBI requests and housekeeping stop. A running Atari crashes, which is
accepted: either main already crashed (that's why there's a core dump), or
the Sidecar hasn't powered the Atari on yet (a firmware update). **One way:**
only a reset of main leaves it. All of main's link service is in
`core0_serviceSidecar()`, called from `core0_utilityTasks()` and from the
startup wait for the 6502's first writes, so main answers before the Atari
runs or with no Atari at all (until that wait's timeout: 30 s on hardware,
1 s with FAKE_CLOCK). The hot bmon loop is untouched.

`SetDiagnostic` carries flags (any flag implies `Diagnostic`; flags only
add up until reset):

| Flag | | |
|---|---|---|
| `Diagnostic` | 0x01 | detach the bus, serve the link |
| `FetchFirmware` | 0x02 | then pull the Sidecar's `Firmware` and install it (OTA; not built yet) |
| `Panic` | 0x04 | then `abort()`: a deliberate crash, to test the core dump path |

**Reset line (planned hardware):** in diagnostic mode main will also hold
the Atari's reset line low, so an Atari left running on a detached bus
can't hurt itself. The current boards have no reset connection.

Sidecar console: `diag`, `diag ota`, `diag panic`.

**Core dump fetch (sketched in, 2026-11-14).** Main registers its core dump
as the `Core` object at boot and serves it only in diagnostic mode
(`Sidecar::pollDiagnostic()`, which reads flash; `poll()` keeps answering
Unavailable). On a new core in the boot report the Sidecar enters
diagnostic mode at once, fetches it, checks the SHA-256 IDF stored in its
last 32 bytes, then prints `SIDECAR HALT: ...` and halts (console only);
main stays in diagnostic mode. Storing the dump (SD card or Sidecar flash)
is not built: until the large-flash boards, core dumps still come off
main over USB (`crash_bundle.sh`).

**Transport testing (until the 16 MB boards):** master keeps the
production layout (`partitions.csv`: no `ota_1`) and full core dumps
(`CAPTURE_DRAM`). The `exp-sidecar-ota` branch, for the bench pair only and
never the test pool, has stack-only core dumps (~11 KB) and a partition
table with both `ota_1` and a core partition, to test OTA and the core
dump fetch together.

**Objects, not files:** a constant enum of things a side can offer. For now:

| Object | Offered by | |
|---|---|---|
| `Firmware` | Sidecar | the main app image to install (main OTA) |
| `Core` | main | main's last core dump |
| `Test` | both | a fixed pattern, for checking fetches both ways (csim, bench) |

**No state in the protocol:** no open, no handles, no sessions. A reader
asks for `ReadVersion`, decides for itself whether to fetch, then asks for
`ReadChunk` at offset 0, 0 + n, ... and stops at a zero-length reply (EOF);
the last data chunk may be short. A retry is always safe: the same object
and offset give the same bytes.

**Version token:** opaque, 32 bits, changes whenever the content changes
(e.g. a CRC32 of the content, or for `Firmware` part of the app image's own
SHA-256). The reader calls `ReadVersion` before and after its fetch and
keeps the data only if the token is the same both times: an object replaced
mid-fetch shows up as a changed token, with no state on the server. Status
values: ok, no such object, not available now, I/O error.

**Rough speed:** a round trip of about 1.5 ms per ~270-byte chunk is
~180 kB/s, so a 970 KB app image in about 5-6 s (estimate, not measured).

## Open (later)

- **Policy:** when to fetch, when to install, who decides. A reader checks
  `ReadVersion` and decides; no automatic reflash for now (the Sidecar's
  serial flasher stays a deliberate console command).
- **Main OTA needs a second app slot:** `partitions.csv` has one app
  partition (`app0`, 1.9 MB); the core dump took the second slot's space.
- **Flash writes on main vs the hot bus:** writing flash stops the cache,
  which core 1 relies on for PSRAM-backed banks; installing may need the
  bus detached or idle.
- **csim:** `i2slink_csim` and both apps change in step.
- **Loopback** (the main board's self-test, RX on its own TX): a side reading
  its own command frame back takes it as the response, as v1 did.
