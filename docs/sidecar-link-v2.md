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

And main to Sidecar, once per boot: `BootStatus` (reset reason, what main
offers: core dump partition and token, `ota_1` slot, app token, git
version). **The Sidecar's reply carries the decision**: the diagnostic
flags main must enter, 0 to boot normally.

**Diagnostic mode only at boot (exp-sidecar-ota, 2026-11-14).** Anything
touching main's flash happens in diagnostic mode: core dump fetches and
firmware updates. Main enters it only from the `BootStatus` reply, in
`setup()`, before core 1 starts or the bus is ever attached: a running
Atari is simply never served (it crashes, which is accepted: either main
just crashed, which is why there's a core dump, or the Sidecar hasn't
powered the Atari on yet). The bmon loop never polls the link at all: no
link code runs beside the hot bus except main's own SIO/CIO/Mount calls.
The Sidecar can't put a running main in diagnostic mode, and doesn't need
to; to force it, it resets main and answers the next boot report. Only a
reset leaves it. (`SetDiagnostic`, command 6, is gone.)

| Flag | | |
|---|---|---|
| `Diagnostic` | 0x01 | don't attach the bus; serve the link until reset |
| `FetchFirmware` | 0x02 | first pull the Sidecar's `Firmware` into the next app slot and restart into it (OTA) |
| `Panic` | 0x04 | first `abort()`: a deliberate crash, to test the core dump path |

The Sidecar's rule, in its `BootStatus` handler: a core dump it hasn't
fetched means main just crashed, so `Diagnostic` (then it fetches the dump,
checks its SHA-256 and halts); plus any flags staged on its console for
that boot (`diag`, `diag ota`, `diag panic`: one shot, applied at main's
next boot report, so reset main to apply them now).

**Finding the Sidecar at boot.** A build with the link expects one.
Development model: main waits up to 3 s (the Sidecar may still be booting
after a common power-on), prints `EXPECTED A SIDECAR, none answered`, and
boots normally without it; the test farm's `boot.sh` brings the Sidecar up
first. On production boards the Sidecar boots main deliberately (no race),
and a missing Sidecar will likely be fatal.

**OTA (works on the bench pair, 2026-11-14).** `SidecarHost::installFirmware()`:
`esp_ota_begin()` on the next slot, each `ReadChunk` straight into
`esp_ota_write()`, kept only if the fetch was complete and consistent (the
same token before and after, the bytes' CRC32 equal to it) and
`esp_ota_end()` accepts the image; then `esp_ota_set_boot_partition()` and
a restart. Core 1 hasn't started, so the flash writes stall nothing. A
994 KB image takes ~22 s (45 kB/s, flash writes included). The Sidecar
stages the image with `sidecar/flash_spiffs.sh` (`/spiffs/main/app.bin`,
from `MAIN_BUILD`).

**Reset line (planned hardware):** in diagnostic mode main will also hold
the Atari's reset line low, so an Atari left unserved can't hurt itself.
The current boards have no reset connection.

**Core dump fetch.** Main registers its core dump as the `Core` object at
boot and serves it only in diagnostic mode (`Sidecar::pollDiagnostic()`,
which reads flash; `poll()` answers Unavailable). Storing the dump (SD card
or Sidecar flash) is not built: until the large-flash boards, core dumps
still come off main over USB (`crash_bundle.sh`).

**Master** still has the older arrangement (the link served in the startup
wait and diagnostic mode via `SetDiagnostic`, the utility-pass call
commented out); this section is the branch's.

**Transport testing (until the 16 MB boards):** master keeps the
production layout (`partitions.csv`: no `ota_1`) and full core dumps
(`CAPTURE_DRAM`). The `exp-sidecar-ota` branch has stack-only core dumps (~11 KB)
and a partition table with both `ota_1` and a core partition, to test OTA
and the core dump fetch together. It may run on the test pool for
Sidecar tests (with Sidecar hardware moved there), but never stays there
for the ongoing soaks: a real crash there would lose the full core dump.

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
