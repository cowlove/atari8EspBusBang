#pragma once

/* i2slink internals: the wire format, shared by the hardware link
 * (i2slink.c) and its CSIM model (i2slink_csim.cpp). Not a public header.
 *
 * Pure functions on memory: the TX cycle layout, the bit-shift decode of a
 * received ring, and the lock search. Nothing here touches hardware. */
#include "i2slink.h"

#include "esp_attr.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- Cycle map (offsets in the TX cycle) ---- */
#define N I2SLINK_CYCLE_BYTES
#define DESC_BYTES (N / I2SLINK_DESC_COUNT)
#define LINE_STRIDE (I2SLINK_HSYNC_BYTES + I2SLINK_LINE_BYTES)
#define LINE_START(k) (I2SLINK_VSYNC_BYTES + I2SLINK_HSYNC_BYTES + (k) * LINE_STRIDE)
#define OFF_TAG LINE_START(0U)
#define OFF_H (OFF_TAG + 4U)       /* sampled first */
#define OFF_PAYLOAD (OFF_H + 4U)
#define OFF_END (LINE_START(I2SLINK_LINE_COUNT - 1U) + I2SLINK_LINE_BYTES)
#define OFF_T (OFF_END - 4U)       /* sampled last */
#define OFF_CRC (OFF_END - 8U)
/* Raw bytes the H..T region decodes from: at most 6 more than its length
 * (one byte either side for the bit shift, plus lane and pair rounding). */
#define REGION_RAW_MAX (OFF_END - OFF_H + 6U)

static_assert(OFF_H % 4U == 0U && OFF_T % 4U == 0U && OFF_CRC % 4U == 0U,
              "stamps and CRC must be aligned words in the TX cycle");

static const DRAM_ATTR uint8_t hsync[I2SLINK_HSYNC_BYTES] = {
    0xffU, 0xffU, 0x12U, 0x34U, 0x56U, 0x78U, 0xa5U, 0x5aU,
};
__attribute__((unused)) static const DRAM_ATTR char TAG_SOURCE[4] = {'L', 'K', 'S', '1'};
__attribute__((unused)) static const DRAM_ATTR char TAG_FOLLOWER[4] = {'L', 'K', 'F', '1'};

/* Payload runs: the pieces of the lines between H and the CRC. */
typedef struct {
    uint16_t offset, length;
} run_t;
#define RUN_COUNT I2SLINK_LINE_COUNT
static DRAM_ATTR run_t runs[RUN_COUNT];

__attribute__((unused)) static void runs_init(void)
{
    size_t total = 0U;
    for (size_t line = 0U; line < I2SLINK_LINE_COUNT; ++line) {
        size_t first = LINE_START(line);
        size_t end = first + I2SLINK_LINE_BYTES;
        if (line == 0U) first = OFF_PAYLOAD;
        if (line == I2SLINK_LINE_COUNT - 1U) end = OFF_CRC;
        runs[line] = (run_t){(uint16_t)first, (uint16_t)(end - first)};
        total += end - first;
    }
    assert(total == I2SLINK_PAYLOAD_BYTES);
    (void)total;
}

/* The TX cycle template: VSYNC zeros, the HSYNC markers, the tag, and
 * H = T = 0 (no version yet; readers ignore sequence 0). */
__attribute__((unused)) static void wire_cycle_init(uint8_t *out, const char tag[4])
{
    memset(out, 0, N);
    for (size_t line = 0U; line <= I2SLINK_LINE_COUNT; ++line) {
        memcpy(out + LINE_START(line) - I2SLINK_HSYNC_BYTES, hsync, I2SLINK_HSYNC_BYTES);
    }
    memcpy(out + OFF_TAG, tag, 4U);
}

/* ---- Decoding: aligned byte j of a raw ring (or ring-layout copy) ---- */
/* Each 16-bit sample goes out MSB first, so wire byte k is memory byte k ^ 1
 * on both ends. The received stream is offset by 8 * lane + phase bits:
 * aligned wire byte j is built from wire bytes j + lane and j + lane + 1. */
static inline IRAM_ATTR size_t raw_high(unsigned lane, size_t j)
{
    return ((((j % N) ^ 1U) + lane) % N) ^ 1U;
}

static inline IRAM_ATTR size_t raw_low(unsigned lane, size_t j)
{
    return ((((j % N) ^ 1U) + lane + 1U) % N) ^ 1U;
}

static inline IRAM_ATTR uint8_t decode_at(const uint8_t *raw, unsigned phase, unsigned lane,
                                          size_t j)
{
    const uint8_t high = raw[raw_high(lane, j)];
    if (phase == 0U) {
        return high;
    }
    const uint8_t low = raw[raw_low(lane, j)];
    return (uint8_t)((high << phase) | (low >> (8U - phase)));
}

static inline IRAM_ATTR bool marker_at(const uint8_t *aligned, size_t m)
{
    for (size_t byte = 0U; byte < I2SLINK_HSYNC_BYTES; ++byte) {
        if (aligned[(m + byte) % N] != hsync[byte]) {
            return false;
        }
    }
    return true;
}

/* ---- Decoding under a lock (phase, lane, marker) ---- */
static inline IRAM_ATTR size_t wire_aligned_of(uint16_t marker, size_t tx_offset)
{
    return (marker + tx_offset - I2SLINK_VSYNC_BYTES) % N;
}

__attribute__((unused)) static IRAM_ATTR uint32_t wire_decode_u32(const uint8_t *raw, unsigned phase,
                                                                  unsigned lane, uint16_t marker,
                                                                  size_t tx_offset)
{
    const size_t j = wire_aligned_of(marker, tx_offset);
    uint32_t value = 0U;
    for (unsigned byte = 0U; byte < 4U; ++byte) {
        value |= (uint32_t)decode_at(raw, phase, lane, j + byte) << (8U * byte);
    }
    return value;
}

/* The internal HSYNC markers are where the lock says: framing holds. */
__attribute__((unused)) static IRAM_ATTR bool wire_framing_ok(const uint8_t *raw, unsigned phase,
                                                              unsigned lane, uint16_t marker)
{
    for (size_t line = 1U; line < I2SLINK_LINE_COUNT; ++line) {
        const size_t j = wire_aligned_of(marker, LINE_START(line) - I2SLINK_HSYNC_BYTES);
        for (size_t byte = 0U; byte < I2SLINK_HSYNC_BYTES; ++byte) {
            if (decode_at(raw, phase, lane, j + byte) != hsync[byte]) {
                return false;
            }
        }
    }
    return true;
}

/* The payload runs, decoded into out (I2SLINK_PAYLOAD_BYTES). runs_init()
 * must have run. */
__attribute__((unused)) static IRAM_ATTR void wire_decode_payload(const uint8_t *raw, unsigned phase,
                                                                  unsigned lane, uint16_t marker,
                                                                  uint8_t *out)
{
    for (size_t run = 0U; run < RUN_COUNT; ++run) {
        const size_t j = wire_aligned_of(marker, runs[run].offset);
        for (size_t byte = 0U; byte < runs[run].length; ++byte) {
            *out++ = decode_at(raw, phase, lane, j + byte);
        }
    }
}

/* Search one snapshot (raw) at all 16 bit offsets, starting from
 * (start_phase, start_lane), for a whole valid frame carrying tag: VSYNC
 * zeros, every line's HSYNC marker, and the tag. On success sets the lock
 * (phase, lane, marker).
 *
 * Anchored, not exhaustive: VSYNC's zero bytes stay zero raw bytes at any
 * bit offset, and no other stretch of the cycle is that long without a
 * nonzero byte (a line is 64 bytes between markers), so the longest run of
 * zero raw bytes ends where the first HSYNC begins. Only the few aligned
 * positions next to that end are tried, at each offset, decoding on
 * demand: a few thousand cycles, where decoding the whole ring at all 16
 * offsets took ~2.4 ms. */
__attribute__((unused)) static IRAM_ATTR bool wire_search_anchored(const uint8_t *raw, const char tag[4],
                                                                   unsigned start_phase, unsigned start_lane,
                                                                   uint8_t *phase_out, uint8_t *lane_out,
                                                                   uint16_t *marker_out)
{
    /* The longest circular run of zero raw bytes, and the index just past it. */
    size_t best = 0U, best_end = 0U, run = 0U;
    for (size_t k = 0U; k < 2U * N; ++k) {
        if (raw[k % N] != 0U) {
            run = 0U;
        } else if (++run > best && run <= N) {
            best = run;
            best_end = (k + 1U) % N;
        }
    }
    if (best < I2SLINK_VSYNC_BYTES - 4U || best == N) {
        return false;   /* no VSYNC (no peer, no clock) or a dead-quiet line */
    }
    for (unsigned step = 0U; step < 16U; ++step) {
        const unsigned lane = (start_lane + step / 8U) & 1U;
        const unsigned phase = (start_phase + step) & 7U;
        /* Aligned j reads raw bytes within a couple of j: try the marker a
         * few positions either side of the run's end. */
        for (int d = -6; d <= 6; ++d) {
            const size_t m = (best_end + N + (size_t)(d + 6) - 6U) % N;
            bool valid = true;
            for (size_t byte = 0U; valid && byte < I2SLINK_HSYNC_BYTES; ++byte) {
                valid = decode_at(raw, phase, lane, m + byte) == hsync[byte];
            }
            for (size_t back = 1U; valid && back <= I2SLINK_VSYNC_BYTES; ++back) {
                valid = decode_at(raw, phase, lane, m + N - back) == 0U;
            }
            for (size_t line = 1U; valid && line <= I2SLINK_LINE_COUNT; ++line) {
                for (size_t byte = 0U; valid && byte < I2SLINK_HSYNC_BYTES; ++byte) {
                    valid = decode_at(raw, phase, lane, m + line * LINE_STRIDE + byte) == hsync[byte];
                }
            }
            for (size_t byte = 0U; valid && byte < 4U; ++byte) {
                valid = decode_at(raw, phase, lane, m + I2SLINK_HSYNC_BYTES + byte) == (uint8_t)tag[byte];
            }
            if (valid) {
                *phase_out = (uint8_t)phase;
                *lane_out = (uint8_t)lane;
                *marker_out = (uint16_t)m;
                return true;
            }
        }
    }
    return false;
}

/* The exhaustive search (the original): decode the whole snapshot at each
 * of the 16 offsets and test every position, with the same checks as the
 * anchored one. ~2.4 ms. aligned is N bytes of scratch. Kept as the
 * fallback for read()'s searches while the anchored search proves itself
 * (a lock only this finds is counted: stats.locks_fallback). */
__attribute__((unused)) static IRAM_ATTR bool wire_search_full(const uint8_t *raw, uint8_t *aligned,
                                                               const char tag[4], unsigned start_phase,
                                                               unsigned start_lane, uint8_t *phase_out,
                                                               uint8_t *lane_out, uint16_t *marker_out)
{
    for (unsigned step = 0U; step < 16U; ++step) {
        const unsigned lane = (start_lane + step / 8U) & 1U;
        const unsigned phase = (start_phase + step) & 7U;
        for (size_t j = 0U; j < N; ++j) {
            aligned[j] = decode_at(raw, phase, lane, j);
        }
        for (size_t m = 0U; m < N; ++m) {
            if (aligned[m] != hsync[0] || !marker_at(aligned, m)) {
                continue;
            }
            bool valid = true;
            for (size_t back = 1U; valid && back <= I2SLINK_VSYNC_BYTES; ++back) {
                valid = aligned[(m + N - back) % N] == 0U;
            }
            for (size_t line = 1U; valid && line <= I2SLINK_LINE_COUNT; ++line) {
                valid = marker_at(aligned, m + line * LINE_STRIDE);
            }
            for (size_t byte = 0U; valid && byte < 4U; ++byte) {
                valid = aligned[(m + I2SLINK_HSYNC_BYTES + byte) % N] == (uint8_t)tag[byte];
            }
            if (valid) {
                *phase_out = (uint8_t)phase;
                *lane_out = (uint8_t)lane;
                *marker_out = (uint16_t)m;
                return true;
            }
        }
    }
    return false;
}

/* The lock search: anchored first, then, if allow_full, the exhaustive one.
 * *fallback is set when only the exhaustive search found it. */
__attribute__((unused)) static IRAM_ATTR bool wire_search(const uint8_t *raw, uint8_t *aligned,
                                                          const char tag[4], unsigned start_phase,
                                                          unsigned start_lane, uint8_t *phase_out,
                                                          uint8_t *lane_out, uint16_t *marker_out,
                                                          bool allow_full, bool *fallback)
{
    *fallback = false;
    if (wire_search_anchored(raw, tag, start_phase, start_lane, phase_out, lane_out, marker_out)) {
        return true;
    }
    if (allow_full &&
        wire_search_full(raw, aligned, tag, start_phase, start_lane, phase_out, lane_out, marker_out)) {
        *fallback = true;
        return true;
    }
    return false;
}
