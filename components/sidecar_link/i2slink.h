#pragma once

/* i2slink: a hands-free bidirectional I2S DMA link between two ESP32-S3s.
 *
 * Each direction is a DMA ring that streams one fixed-size wire cycle
 * continuously, with no CPU help and no interrupts. Each end writes whole
 * payloads into its TX cycle and reads whole payloads out of its RX ring.
 * Contract. The link carries the latest value, not a queue, and promises
 * nothing end to end: a version may be overwritten before it is sent, or
 * lost to a tear, a bit error or a lock loss, and the writer is never told.
 * What read() guarantees is narrower: a NEW payload is one complete version
 * exactly as written, never one this reader has returned before, and,
 * within one peer session, newer than the last. A gap in the sequence shows
 * missed versions. A peer restart begins a new session: each init() starts
 * the writer's sequence at a random value, and read() takes any sequence
 * other than the last one it returned as new, so a restarted peer's
 * versions are delivered at once (they are lost only if a restart lands
 * exactly on that number, about 1 in 2^32). Lock losses do not reset this:
 * a relock never redelivers the last version. Build reliable delivery on
 * top if needed, e.g. by echoing the peer's sequence back in your payload.
 *
 * Torn frames are impossible by construction:
 *   - the writer stamps each version at both ends of the payload (H first in
 *     the wire cycle, T last) and writes T first, then the payload and its
 *     CRC32, then H, so one ascending pass with H == T holds one version;
 *   - the reader copies the stamped region only while its RX DMA is
 *     elsewhere (in VSYNC), proven by two "tripwires": the length fields
 *     the DMA writes back into the RX descriptors where the region ends (E)
 *     and just before it starts (the guard, G). Racing the beam;
 *   - the CRC32 rejects wire bit errors.
 *
 * Wiring: BCLK plus one data wire per direction, no WS. One end is the clock
 * source (drives BCLK), the other the follower. Each end uses one I2S
 * peripheral (two simplex channels on it), chosen in the config; a chip
 * with two can host both ends of a loopback test.
 *
 * The client's hot loop:
 *
 *     for (;;) {
 *         hot_work();
 *         if (i2slink_hint(link) &&
 *             i2slink_read(link, payload, &sequence, 0) == I2SLINK_NEW) {
 *             consume(payload, sequence);
 *         }
 *     }
 *
 * Calls: i2slink_init(), i2slink_deinit() and i2slink_stats() are setup
 * calls (driver, allocation, logging). i2slink_write(), i2slink_hint() and
 * i2slink_read() are real-time calls: memory only, in IRAM, no locks, safe
 * with interrupts disabled. hint() and read() share state and must be
 * called from one context; write() may run in any single other context
 * (e.g. the other core). */
#include "esp_err.h"
#include "esp_idf_version.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Wire layout and speed: compile-time, must match on both ends ----
 * One wire cycle: VSYNC (zeros), then LINE_COUNT x (HSYNC marker, line),
 * then a closing HSYNC marker. The payload fills the lines, bracketed by a
 * tag and the H stamp at the front and the CRC32 and T stamp at the back. */
#define I2SLINK_LINE_BYTES     64U
#define I2SLINK_LINE_COUNT     5U
#define I2SLINK_HSYNC_BYTES    8U
#define I2SLINK_VSYNC_BYTES    100U
#define I2SLINK_DESC_COUNT     13U   /* DMA descriptors per cycle, each direction */
#define I2SLINK_CYCLE_BYTES                                                       \
    (I2SLINK_VSYNC_BYTES + (I2SLINK_LINE_COUNT + 1U) * I2SLINK_HSYNC_BYTES +      \
     I2SLINK_LINE_COUNT * I2SLINK_LINE_BYTES)                     /* 468 */
#define I2SLINK_OVERHEAD_BYTES 16U   /* tag, H, CRC32, T */
#define I2SLINK_PAYLOAD_BYTES                                                     \
    (I2SLINK_LINE_COUNT * I2SLINK_LINE_BYTES - I2SLINK_OVERHEAD_BYTES) /* 304 */

/* I2S frames (two 16-bit slots) per second; BCLK = 32 x this. Must divide
 * 5,000,000 (up to 312,500, from the 160 MHz I2S source) or, on ESP-IDF 6.x
 * only, 7,500,000 (up to 468,750, from the 240 MHz source, which IDF 5.x
 * does not offer). The bench measurements were at 375,000 on IDF 6.1. */
#ifndef I2SLINK_FRAMES_PER_SECOND
#define I2SLINK_FRAMES_PER_SECOND 200000U
#endif
/* Sample RX data on the falling BCLK edge (1) instead of the rising one
 * (0), at both ends of the link: like the frame rate, the two peers must
 * agree, so it is set here rather than per config. The bench pairs at
 * 375,000 wanted falling (an S3 slave resamples BCLK through its module
 * clock, so it samples late in the bit). Rising came in with the drop to
 * 200,000 that got the real Sidecar working (531df73); re-measure both
 * together when the rate changes. */
#ifndef I2SLINK_RX_BCLK_INVERT
#define I2SLINK_RX_BCLK_INVERT 0
#endif

/* One wire cycle, in microseconds (rounded down): 312 at 375,000. */
#define I2SLINK_CYCLE_US (I2SLINK_CYCLE_BYTES * 1000000U / (4U * I2SLINK_FRAMES_PER_SECOND))

typedef enum {
    I2SLINK_CLOCK_SOURCE,    /* drives BCLK */
    I2SLINK_CLOCK_FOLLOWER,  /* takes BCLK from the source */
} i2slink_clock_t;

typedef struct {
    i2slink_clock_t clock;
    int i2s_port;             /* I2S_NUM_0 or I2S_NUM_1: both channels live on it */
    int bclk_gpio;            /* source: output; follower: input */
    int tx_gpio;              /* this end's data out */
    int rx_gpio;              /* this end's data in */
    /* (The RX sampling edge is I2SLINK_RX_BCLK_INVERT, above.) */
    /* Link-fault timeout: how long read() keeps trying while it gets no
     * clean copy at all (no lock, no beam, torn or CRC-failing copies)
     * before returning that fault. Internal to framing; the client's wait
     * for new data is read()'s own argument. 0: four cycles. */
    uint32_t sync_timeout_us;
    /* Loopback test: RX hears this end's own TX (rx_gpio == tx_gpio, or a
     * jumper), so read() returns exactly what write() wrote. Use the clock
     * source role (nothing else drives BCLK), with no peer on the wires. */
    bool loopback;
} i2slink_config_t;

typedef enum {
    I2SLINK_NEW,       /* a new version was copied out */
    I2SLINK_TIMEOUT,   /* link healthy, but no new version within wait_us */
    /* Link faults, after sync_timeout_us without a clean copy: */
    I2SLINK_TORN,      /* only torn copies: the writer never paused for a pass */
    I2SLINK_CRC,       /* copies failing their CRC: bit errors */
    I2SLINK_NO_LOCK,   /* no frame found (no peer yet, or framing lost) */
    I2SLINK_STALLED,   /* the RX DMA did not advance: no clock */
} i2slink_status_t;

typedef struct {
    uint32_t writes;          /* versions written */
    uint32_t hints_true;      /* hint() returned true */
    uint32_t reads_new, reads_timeout, reads_torn, reads_crc, reads_no_lock, reads_stalled;
    uint32_t immediate;       /* reads that found the beam window already open */
    uint32_t waited;          /* reads that armed the tripwires and waited */
    uint32_t windows;         /* region copies completed */
    uint32_t late;            /* windows missed: both tripwires crossed */
    uint32_t tripped;         /* copies abandoned: the DMA reached the guard */
    uint32_t torn;            /* copies with H != T (writer mid-update) */
    uint32_t stale;           /* clean copies of an already returned version */
    uint32_t crc_errors;      /* copies failing their CRC */
    uint32_t skipped;         /* estimate: versions between consecutive NEW reads */
    uint32_t jumps;           /* sequence discontinuities (peer restarts) */
    uint32_t framing;         /* copies with broken framing (lock dropped) */
    uint32_t locks;           /* locks acquired */
    uint32_t peeks;           /* peek() calls */
    uint32_t peeks_torn;      /* peek() copies torn or failing their CRC (retried later) */
    uint8_t bit_phase, byte_lane;   /* current lock */
    uint16_t marker_offset;
} i2slink_stats_t;

typedef struct i2slink i2slink_t;

/* Start both rings and return; the lock is found by the first read().
 * Either end may start first or restart at any time; the other relocks. */
esp_err_t i2slink_init(i2slink_t **link, const i2slink_config_t *config);
void i2slink_deinit(i2slink_t *link);

/* Publish a new version. Never blocks; replaces any version not yet sent.
 * A writer that never pauses for a whole wire cycle can tear every pass:
 * leave the reader some gaps. */
void i2slink_write(i2slink_t *link, const uint8_t payload[I2SLINK_PAYLOAD_BYTES]);

/* Performance hint, nothing more: true when it is worth stepping out of the
 * hot loop to call i2slink_read(). Usually two loads. A true may be wrong
 * (read(..., 0) then returns TIMEOUT); a new version can be missed for at most a
 * descriptor time or so, never for good: the hint stays true until a read()
 * succeeds. It is always true while unlocked, so the first read() can
 * search. As a favour to read() it keeps the tripwires armed, so a read()
 * that follows a true hint promptly usually needs no wait; call it as often
 * or as rarely as suits the loop. */
bool i2slink_hint(i2slink_t *link);

#define I2SLINK_WAIT_FOREVER UINT32_MAX

/* Copy out the newest version, if it is one this reader has not returned:
 * find the lock if needed (about 0.5 ms), wait for the beam window (at most
 * a cycle), copy, check the stamps and CRC, retrying failed copies.
 *
 * Two timeouts:
 *
 *   wait_us, the client's: how long to wait for a version newer than the
 *   last one returned, while the link is healthy. Past it, read() returns
 *   TIMEOUT. I2SLINK_WAIT_FOREVER never times out for lack of news.
 *
 *   wait_us == 0: check what the link holds now, without waiting for
 *   anything newer to arrive. If it is the version last returned (nothing
 *   new), read() returns TIMEOUT and leaves payload untouched; if it is a
 *   version not yet returned, NEW. It is still not instant: checking takes
 *   one clean copy, which can mean waiting for the beam window (up to about
 *   one wire cycle, 312 us at the default speed) and retrying a torn copy.
 *   This is the call to make after a true hint(). There is no strictly
 *   non-blocking read: hint() is the non-blocking check.
 *
 *   config.sync_timeout_us, the link's: after that long without any clean
 *   copy, read() returns the fault (TORN, CRC, NO_LOCK, STALLED) whatever
 *   wait_us says.
 *
 * So read() returns within about wait_us + sync_timeout_us.
 *
 * On NEW, payload holds the version and sequence (may be NULL) its number;
 * consecutive versions differ by 1. Otherwise payload is unchanged. */
i2slink_status_t i2slink_read(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                              uint32_t *sequence, uint32_t wait_us);

/* The poll: one look, never waits, never paces. Returns NEW (payload and
 * sequence as read()), TIMEOUT (nothing new complete now, or a copy torn
 * by a version arriving that instant: look again later) or NO_LOCK.
 *
 * With a lock: if T (the frame's last field) hasn't changed since the last
 * version returned, TIMEOUT at once (a couple of loads; at most once per
 * half wire cycle also a check of the live HSYNC markers, which drops a
 * stale lock). Otherwise one copy
 * of the region straight away, no beam window: the H and T stamps and the
 * CRC reject a torn copy. Without a lock: one search of the ring for the
 * frame (at most one per half wire cycle; else NO_LOCK at once), then as
 * above. For callers that come back often anyway (a periodic poll, a
 * service loop); read() is for waiting. Real-time, like read(). */
i2slink_status_t i2slink_peek(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                              uint32_t *sequence);

/* Debug (setup-level, logs): hex dump of the RX ring and a few descriptors. */
void i2slink_debug_dump(const i2slink_t *link, const char *name);

/* Snapshot of the counters. */
void i2slink_stats(const i2slink_t *link, i2slink_stats_t *stats);

/* The link's memory: one block holding everything its polling and DMA touch
 * (buffers and descriptors), for checking which SRAM bank it shares. */
void i2slink_memory(const i2slink_t *link, uintptr_t *start, size_t *size);

/* The span of the link's static tables (the hsync pattern, the payload runs
 * and the end tags), shared by every link: read and written about once per
 * frame, by i2slink_read() and i2slink_write(). For the same bank check. */
void i2slink_static_memory(uintptr_t *start, size_t *size);

#ifdef __cplusplus
}
#endif
