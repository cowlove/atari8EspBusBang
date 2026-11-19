# i2slink

A hands-free, bidirectional link between two ESP32-S3s over I2S DMA: three wires, payloads of up to 292 bytes (or about 1.7 KB if you build it larger), and no CPU or interrupt involvement on the wire path.

Each end runs one I2S peripheral with its two channels in simplex: TX streams a fixed-size *wire cycle* out of a looping DMA ring, and RX streams the peer's cycles into another. Both DMAs run forever on their own. Sending is a memory write into the TX ring. Receiving is a copy out of the RX ring, timed so the DMA can't overwrite the region being copied. Nothing on the wire path takes a lock, an interrupt or a FreeRTOS call, so the calls are safe in a hot loop, from IRAM, with interrupts disabled.

It was written for [atari8EspBusBang](https://github.com/cowlove/atari8EspBusBang). There, one ESP32-S3 core serves an Atari 8-bit computer's 6502 bus cycle by cycle, and the other core has to talk to a second ESP32-S3 without disturbing it: no interrupts, no peripheral register traffic in the hot loop, no hammering of shared SRAM.

## Features

- **Three wires:** BCLK, plus one data line each way (and ground). No WS, no CS, no handshake lines. One end drives BCLK (the *clock source*), the other follows.
- **Latest value, not a queue.** `i2slink_write()` publishes a new version and never blocks. `i2slink_read()` returns a version only once, complete and exactly as written, with a sequence number so gaps show. Reliable delivery is for the layer on top, e.g. echo the peer's sequence in your payload.
- **No torn frames, by construction:**
  - Each version is stamped at both ends, and the writer writes the stamps in an order that makes a half-written frame detectable.
  - The reader copies only while the RX DMA is provably elsewhere: the descriptors' write-back length fields act as tripwires. This is "racing the beam", as on a raster display.
  - A CRC32 rejects bit errors.
- **Self-framing:**
  - The receiver finds the frame (bit phase, byte lane and position in its ring) by itself.
  - Either end can start first, or restart at any time; the other end relocks.
  - Each end reports its lock state to the other inside its frames.
- **Variable-length frames:** a short version ends early in the wire cycle, so it arrives and is copied sooner.
- **TX auto sync and slot tuning** (optional, on one end):
  - That end shifts its TX stream until the peer receives it aligned, so the peer decodes with plain copies.
  - It then moves its frame to just after the moment its fast replies are usually ready, which cuts the reply's wait for its turn in the stream.
  - Moves are announced over the link first, so the peer follows without losing lock.
- **Real-time calls:**
  - `i2slink_write()`, `i2slink_hint()`, `i2slink_read()` and `i2slink_peek()`: memory only, IRAM, no locks.
  - `hint()` is a couple of loads and says whether a `read()` is worth making.
  - `peek()` never waits.

## Performance

Measured with ESP-IDF 6.1 at the default 312,500 frames/s (BCLK 10 MHz; a 468-byte wire cycle every 374 µs; 292-byte payload):

| | min | mean | max |
|---|---|---|---|
| Loopback, one hop: write to read back (one board, [ping-pong example](examples/pingpong)) | 198 µs | 377 µs | 573 µs |
| Round trip between two boards, request to reply, slot tuning on (jumper-wired pair, inside atari8EspBusBang's protocol) | 442 µs | 641 µs | 816 µs |

- **Throughput:** 390 kB/s of 292-byte echoes in loopback. Between two boards, a request/response protocol moving 281-byte chunks reaches 233.6 kB/s: three wire cycles a chunk, limited by the application's own work per chunk.
- **Where a hop's time goes:**
  - ~58 µs: the frame itself on the wire.
  - ~147 µs: the I2S TX FIFO. The GDMA keeps it full, so every byte sits behind about 184 others. There's no setting for its depth; only a faster wire rate shortens it.
  - 0 to 1 wire cycle: waiting for this end's slot in the TX stream. The writer decides when to write, so this part is random.
- **Wire rate:** 312,500 frames/s ran without data errors on jumper wires. At 375,000 and 468,750 the link still passed data, but it relocked more often. Short PCB traces should do better.

## Hardware

- **ESP32-S3 only.** The link reads the GDMA's descriptor registers and the I2S driver's channel internals directly.
- **ESP-IDF 6.x, tested on 6.1.** The code still has its ESP-IDF 5.x paths, but they haven't been tested lately. Rates above 312,500 frames/s need IDF 6, for the 240 MHz I2S clock source.
- **Per end:** one I2S port (`i2s_port` in the config) and about 3 KB of internal, DMA-capable RAM for the rings and state. The TX auto sync end needs one more wire-cycle buffer.
- **Two links on one chip:** a chip with two I2S ports can run both ends of a link, as a loopback test.

## Wiring

| Clock source | | Clock follower |
|---|---|---|
| `bclk_gpio` (out) | → | `bclk_gpio` (in) |
| `tx_gpio` | → | `rx_gpio` |
| `rx_gpio` | ← | `tx_gpio` |
| GND | — | GND |

Any GPIOs the I2S matrix can reach will do.

## Usage

```c
#include "i2slink.h"

static i2slink_t *link;

void start_link(void)
{
    const i2slink_config_t config = {
        .clock = I2SLINK_CLOCK_SOURCE,   /* the other end: I2SLINK_CLOCK_FOLLOWER */
        .i2s_port = 0,
        .bclk_gpio = 4,
        .tx_gpio = 5,
        .rx_gpio = 6,
        .sync_timeout_us = 0,            /* default: four wire cycles */
        .loopback = false,
        .tx_auto_sync = false,           /* true on (at most) one end */
        .log = NULL,                     /* printf */
    };
    ESP_ERROR_CHECK(i2slink_init(&link, &config));
}

void hot_loop(void)
{
    uint8_t payload[I2SLINK_PAYLOAD_BYTES];
    uint32_t sequence;
    for (;;) {
        do_realtime_work();
        if (i2slink_hint(link) && i2slink_read(link, payload, &sequence, 0) == I2SLINK_NEW) {
            handle(payload, i2slink_last_length(link), sequence);
        }
    }
}

void send(const uint8_t *message, size_t length)   /* any one context */
{
    i2slink_write(link, message, length);
}
```

- **Calling contexts:** `hint()` and `read()`/`peek()` share state and must be called from one context. `write()` may be called from any one context, the same one or another (another core, say).
- **Waiting:** `read()` with a nonzero `wait_us` waits for a new version.
- **Status codes:** besides `NEW` and `TIMEOUT`, `read()` returns link faults: `NO_LOCK`, `TORN`, `CRC` and `STALLED`.
- **Statistics:** `i2slink_stats()` returns counters: locks, torn copies, CRC errors, skipped versions, slot tuning and more.

The full contract is in [include/i2slink.h](include/i2slink.h).

## Configuration

Compile-time settings. Set them for the component with the build's compile definitions, before `project()` in your top-level `CMakeLists.txt`:

```cmake
idf_build_set_property(COMPILE_DEFINITIONS "I2SLINK_LINE_COUNT=12" APPEND)
```

**These must match on both ends** (they define the wire format):

| Setting | Default | Meaning |
|---|---|---|
| `I2SLINK_LINE_COUNT` | 5 | Frame size. The wire cycle is 108 + 72 × n bytes and the payload 64 × n − 28: 5 gives 292 bytes, 12 gives 740, 20 gives 1,252, 27 gives 1,700. |
| `I2SLINK_FRAMES_PER_SECOND` | 312500 | Wire rate; BCLK is 32 × this. Must divide 5,000,000 (or 7,500,000 on IDF 6). |
| `I2SLINK_RX_BCLK_INVERT` | 0 | Sample RX on BCLK's falling edge instead of its rising one. |

**These are per end:**

| Setting | Default | Meaning |
|---|---|---|
| `I2SLINK_TX_AUTO_SYNC_CODE` | 1 | Build the TX auto sync and slot tuning code. 0 saves IRAM on an end that never sets `tx_auto_sync`. |
| `I2SLINK_HOT_SECTION` | (unset) | Put the polling loops in this section instead of plain IRAM, for projects that place their hot code with a linker fragment. |
| `I2SLINK_POLL_DELAY` | 200 | Idle-poll pacing, in loop iterations of about 2 CPU cycles each. Keeps polling from saturating SRAM shared with the other core. |

## Installation

Copy (or `git submodule add`) this directory into your project's `components/`. The [ping-pong example](examples/pingpong) uses it from the component manager, through `override_path`.

## Caveats

- **Driver internals:** it includes ESP-IDF driver-private headers (`i2s_private.h`, `gdma_priv.h`), so a new ESP-IDF release can break it. Tested on 6.1.
- **No version negotiation:** the wire format has changed and may change again. Run the same version on both ends.
- **Diagnostics in the API:** the `i2slink_test_*` and `i2slink_debug_dump()` calls exist for testing and benchmarking.

## License

GPL-2.0-only. See [LICENSE](LICENSE).
