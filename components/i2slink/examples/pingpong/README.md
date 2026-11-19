# i2slink ping-pong

Round-trip latency and echo throughput over [i2slink](../../README.md), between two ESP32-S3 boards, or on one board with no wiring at all.

- **PING** drives the link clock, sends pings and waits for each echo. It runs in rounds:
  - **latency:** 2,000 16-byte pings, each sent at a random moment, as real requests are. Prints min/mean/max round trip and a histogram.
  - **bulk:** 3 s of back-to-back pings at the bulk size (292 bytes by default), each echo checked byte for byte. Prints the echo rate.
- **PONG** follows the clock and echoes each ping back. It also runs the link's slot tuning, which moves its frame to just after the moment its replies are ready.
- **LOOPBACK** needs one board and no wires. RX listens on the board's own TX pin, so each ping comes back as itself. This times the link's one-way pipeline and checks the board's I2S, DMA and the library together.

## Wiring (PING / PONG)

| PING (clock source) | | PONG (clock follower) |
|---|---|---|
| GPIO4 BCLK (out) | → | GPIO4 BCLK (in) |
| GPIO5 TX | → | GPIO6 RX |
| GPIO6 RX | ← | GPIO5 TX |
| GND | — | GND |

The pins can be changed in `idf.py menuconfig` → *i2slink ping-pong*. Keep the wires short: the defaults run BCLK at 10 MHz.

## Build and run

```sh
cd examples/pingpong
idf.py set-target esp32s3
idf.py menuconfig          # i2slink ping-pong -> This board's role
idf.py -p PORT flash monitor
```

Flash one board as PONG and the other as PING. PING waits until the link is up.

## Sample output

LOOPBACK on an ESP32-S3-DevKitC-1 (N16R2), ESP-IDF 6.1, default settings (312,500 frames/s, 468-byte wire cycle of 374 µs, 292-byte payload):

```
i2slink ping-pong: LOOPBACK, BCLK GPIO4, TX GPIO5, RX GPIO5; 312500 frames/s, 292-byte payload, 468-byte wire cycle (374 us)
PING: link up
round 1, latency (16-byte pings at random times): 2000 ok, 0 lost; round trip us min 198 mean 377 max 573
round 1, bulk (292-byte pings): 4007 echoed, 0 lost, 390.0 kB/s each way; echo mismatches so far 0
PING link: writes 6008, locks 1, torn 4162, CRC errors 0, skipped 0, slot tunes 0
```

Loopback is one hop. Its spread, about one wire cycle, is the ping waiting for its turn in the TX stream. Two boards add the second hop and the echo. Bench numbers for that are in the [component README](../../README.md#performance).

`torn` counts copies the reader abandoned and retried because the frame was being rewritten under it. Torn copies are never returned: retrying them is how the link works.
