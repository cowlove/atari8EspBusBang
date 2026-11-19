#include "a8log.h"
#include "i2slink.h"

#include "i2s_dma_ring.h"
#include "i2slink_wire.h"
#include "sidecar_hot.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "i2slink";

/* The cycle map, payload runs and decode are in i2slink_wire.h. */
_Static_assert(N % I2SLINK_DESC_COUNT == 0U && DESC_BYTES % 4U == 0U,
               "the cycle must split into equal word-sized descriptors");
_Static_assert(I2SLINK_DESC_COUNT <= I2S_DMA_RX_PROGRESS_MAX_DESC, "too many descriptors");
/* Beam racing needs, outside the region, the descriptor that ends the
 * window plus a whole spare descriptor before the region: two whole
 * descriptors in the gap, wherever it falls. */
_Static_assert(N - REGION_RAW_MAX >= 3U * DESC_BYTES - 1U,
               "VSYNC too short for beam racing with these descriptors");

#if I2SLINK_FRAMES_PER_SECOND <= 312500U && 5000000U % I2SLINK_FRAMES_PER_SECOND == 0U
#define CLOCK_SOURCE I2S_CLK_SRC_DEFAULT      /* PLL_F160M */
#define CLOCK_SOURCE_MHZ 160
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) && I2SLINK_FRAMES_PER_SECOND <= 468750U && \
    7500000U % I2SLINK_FRAMES_PER_SECOND == 0U
#define CLOCK_SOURCE I2S_CLK_SRC_PLL_240M
#define CLOCK_SOURCE_MHZ 240
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#error "I2SLINK_FRAMES_PER_SECOND must divide 5,000,000 (<= 312,500) or 7,500,000 (<= 468,750)"
#else
#error "I2SLINK_FRAMES_PER_SECOND must divide 5,000,000 (<= 312,500) on ESP-IDF 5.x (no 240 MHz I2S source)"
#endif

#define LEN_MASK 0x00FFF000U    /* RX descriptor word 0: the length field */
#define CPU_MHZ ((uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)
#if defined(__XTENSA__)
#define MEMW() __asm__ volatile("memw" ::: "memory")
#else
#define MEMW() __atomic_thread_fence(__ATOMIC_SEQ_CST)   /* csim (host builds) */
#endif
/* The rest after a failed search, in sidecar_nez_delay() iterations (about
 * two cycles each): one wire cycle. */
#define SEARCH_BACKOFF_ITERATIONS (I2SLINK_CYCLE_US * CPU_MHZ / 2U)
/* Half a wire cycle in CPU cycles: peek()'s least gap between lock
 * searches (each decodes the snapshot at 16 offsets: SRAM traffic beside
 * the core 1 bus loop), and peek()'s and hint()'s between frame sync
 * checks. */
#define PEEK_SEARCH_GAP_CYCLES (I2SLINK_CYCLE_US * CPU_MHZ / 2U)

struct i2slink {
    /* DMA memory (the struct is allocated DMA-capable, 16-aligned). */
    uint8_t tx_cycle[N] __attribute__((aligned(16)));
    uint8_t rx_ring[N] __attribute__((aligned(16)));
    uint8_t search_raw[N] __attribute__((aligned(16)));
    uint8_t search_aligned[N];

    /* The DMA descriptors, here rather than scattered over the heap: the
     * poll reads RX's and the DMA writes them back every cycle. */
    lldesc_t rx_desc[I2SLINK_DESC_COUNT];
    lldesc_t tx_desc[I2SLINK_DESC_COUNT];

    i2s_chan_handle_t tx, rx;
    i2s_dma_segment_t segments[I2SLINK_DESC_COUNT];
    volatile uint32_t *dw0[I2SLINK_DESC_COUNT];
    uint32_t dw0_armed[I2SLINK_DESC_COUNT];   /* word 0 with length 0 */
    char rx_tag[4];
    uint32_t sync_timeout_cycles;
    uint32_t last_peek_search;   /* CCOUNT of peek()'s last lock search */
    uint32_t last_peek_framing;  /* CCOUNT of peek()'s last live framing check */
    uint32_t last_hint_framing;  /* CCOUNT of hint()'s last frame sync check */
    bool peek_searched;

    /* Writer */
    uint32_t tx_sequence;
    /* TX alignment (tx_auto_sync): the unshifted cycle; tx_cycle is it shifted
     * by tx_shift bits. NULL without tx_auto_sync (tx_cycle is the cycle). */
    uint8_t *tx_logical;
    uint32_t tx_shift;
    /* The peer report generation last seen (0x100: none), and tx_shift when
     * it was first seen: the shift it was measured at. */
    uint16_t seen_gen;
    uint32_t seen_shift;
    int32_t sync_marker;    /* -1: sync the bit phase and lane only; else the peer's marker too (a test) */
    uint8_t report_gen;     /* this end's lock report generation */
    /* Announced moves ("Announced moves" below). The mover (tx_auto_sync):
     * its announcement (move_seq 1..3, 0: none) of a move move_d samples
     * earlier, made at CCOUNT move_since; the next number to use. The
     * follower: the announcement it echoes, the move armed, and since when
     * its framing has been failing with it armed. */
    uint8_t move_seq, move_next_seq, move_d;
    uint32_t move_since;
    uint8_t echo_seq, follow_d;
    bool follow_armed, follow_failing;
    uint32_t follow_fail_since;
    /* Slot tuning (tx_auto_sync): the TX DMA's position as last seen
     * changing descriptor (tx_ref_desc entered at about CCOUNT tx_ref_cc),
     * the CCOUNT of the last NEW read, and the window of waits (bits). */
    bool slot_tune;
    bool slot_trace;          /* print each window (i2slink_test_slot_config()) */
    uint32_t slot_fast_cycles;
    bool tx_ref_ok;
    int8_t tx_seen_desc;
    uint8_t tx_ref_desc;
    uint32_t tx_seen_cc, tx_ref_cc;
    uint32_t tx_ref_err;    /* its uncertainty then, in cycles (half the polls' gap) */
    bool new_cc_ok;
    uint32_t new_cc;
    uint8_t slot_n;
    uint16_t slot_proposal;   /* the last window's proposed move (bits earlier; 0: none) */
    uint16_t slot_wait[32];
    uint16_t slot_resp[32];

    /* Reader: lock */
    bool locked;
    uint8_t phase, lane;
    uint16_t marker;
    uint16_t region_start, region_length;
    /* Tripwires (RX descriptor indexes): the region's first and last
     * (at the most length), the one holding the length word (the front),
     * the guard G, and per stream word count (L4 / 4) the one holding T: E. */
    uint8_t first_desc, max_end_desc, front_desc, cur_end_desc;
    uint8_t end_desc_of[STREAM_WORDS];
    volatile uint32_t *guard_dw0;
    uint32_t guard_armed;
    /* The length of the version last returned (NEW). */
    uint16_t last_length;
    /* hint(): the raw ring bits of H's low byte, as of the last good read */
    const volatile uint32_t *h_word[2];
    uint32_t h_mask[2], h_bits[2];
    /* Locked (or redelivered) with no clean copy since: whatever is in the
     * ring counts as new, so hint() and peek() look. */
    bool t_unread;

    uint32_t last_read;
    bool have_read;

    i2slink_stats_t stats;
};

/* Everything the link's polling and DMA touch is in this one struct. Aligned
 * to LINK_ALIGN (the smallest power of two that holds it: a divisor of the
 * 32 KB SRAM bank), it can't straddle a bank, so all of it lands in exactly
 * one. It grows with the frame (about 5 cycles: I2SLINK_LINE_COUNT). */
#define LINK_ALIGN                                                                \
    (sizeof(struct i2slink) <= 4096U ? 4096U : sizeof(struct i2slink) <= 8192U ? 8192U \
     : sizeof(struct i2slink) <= 16384U ? 16384U : 32768U)
_Static_assert(sizeof(struct i2slink) <= 32768U, "i2slink_t no longer fits one 32 KB SRAM bank");

/* ------------------------------------------------------------------------ */
/* Decoding: aligned byte j of a raw ring (or ring-layout copy) under a lock */
/* ------------------------------------------------------------------------ */

static inline IRAM_ATTR size_t aligned_of(const i2slink_t *link, size_t tx_offset)
{
    return wire_aligned_of(link->marker, tx_offset);
}

static inline IRAM_ATTR uint32_t decode_u32(const i2slink_t *link, const uint8_t *raw,
                                            size_t tx_offset)
{
    return wire_decode_u32(raw, link->phase, link->lane, link->marker, tx_offset);
}

/* ------------------------------------------------------------------------ */
/* Writer                                                                   */
/* ------------------------------------------------------------------------ */

static void tx_cycle_init(i2slink_t *link, const char tag[4])
{
    uint8_t *out = link->tx_cycle;
    wire_cycle_init(out, tag);
    for (size_t index = 0U; index < I2SLINK_DESC_COUNT; ++index) {
        link->segments[index] = (i2s_dma_segment_t){out + index * DESC_BYTES, DESC_BYTES};
    }
}

/* ---- TX alignment ----
 * The idea: main's RX decode (a shift and merge of two raw bytes for
 * every byte, at an arbitrary bit phase and lane) becomes a plain copy if
 * the Sidecar shifts its TX stream so main hears it at phase 0, lane 0.
 * Main drives BCLK, so the offset main hears is fixed until either end
 * restarts: the Sidecar can correct for it once.
 *
 * Every end reports its RX lock in its TX frames (OFF_REPORT: the delay D
 * in bits from the peer's TX cycle to this end's RX ring, a generation
 * bumped on each new lock, a check byte). An tx_auto_sync end keeps its
 * unshifted cycle in tx_logical and renders it into tx_cycle delayed by
 * tx_shift bits; when the peer reports a bit phase or lane (D mod 16)
 * other than 0 it moves tx_shift back by that much and re-renders. That
 * breaks the peer's lock; the peer relocks, now aligned, with a new
 * generation. A report counts as measured at the shift this end had when
 * it first saw that generation, and is acted on only at that shift: once
 * acted on, it predates the new one. (A rule of "never act on a generation
 * twice" deadlocked in csim's lockstress align: the shift came back to the
 * one an idle peer's last, unaligned lock was measured at, the peer's lock
 * held, so no new generation ever came.) */
/* The auto-sync end's code (rendering a shifted TX, slot tuning, making
 * announced moves) is only
 * built where it can be used: main's build sets I2SLINK_TX_AUTO_SYNC_CODE
 * to 0 (it never syncs; it reports, as every end does), which keeps ~1 KB
 * out of its IRAM, where it would push .data, core 0's interrupt stack
 * with it, into core 1's hot data bank (verify_core1_hot_data.py). With it
 * 0, tx_auto_sync fails init. */
#ifndef I2SLINK_TX_AUTO_SYNC_CODE
#define I2SLINK_TX_AUTO_SYNC_CODE 1
#endif
#define TX_SYNC(link) (I2SLINK_TX_AUTO_SYNC_CODE && (link)->tx_logical != NULL)

#define REPORT_NO_LOCK 0xFFFFU
#define CYCLE_BITS (8U * N)
_Static_assert(CYCLE_BITS < REPORT_NO_LOCK, "the delay must fit the report's 16 bits");

static inline IRAM_ATTR uint32_t report_encode(uint32_t delay, uint32_t gen)
{
    const uint32_t v = (delay & 0xFFFFU) | ((gen & 0xFFU) << 16);
    const uint32_t check = 0x5AU ^ (v & 0xFFU) ^ ((v >> 8) & 0xFFU) ^ ((v >> 16) & 0xFFU);
    return v | (check << 24);
}

static inline IRAM_ATTR bool report_decode(uint32_t v, uint32_t *delay, uint32_t *gen)
{
    const uint32_t check = 0x5AU ^ (v & 0xFFU) ^ ((v >> 8) & 0xFFU) ^ ((v >> 16) & 0xFFU);
    if ((v >> 24) != check) {
        return false;
    }
    *delay = v & 0xFFFFU;
    *gen = (v >> 16) & 0xFFU;
    return true;
}

/* This end's lock as a delay in bits: the peer's TX wire bit b is this
 * ring's wire bit b + D (mod the cycle). Aligned byte j of the lock is
 * the peer's TX byte j - d (d = marker - VSYNC, even in any valid lock),
 * whose wire bits start lane bytes and phase bits further on. */
static IRAM_ATTR uint32_t lock_delay(const i2slink_t *link)
{
    const uint32_t d = ((uint32_t)link->marker + N - I2SLINK_VSYNC_BYTES) % N;
    if ((d & 1U) != 0U) {
        return REPORT_NO_LOCK;
    }
    return 8U * ((d + link->lane) % N) + link->phase;
}

/* Logical wire byte u (any integer mod N) of the unshifted cycle. */
__attribute__((unused)) static inline IRAM_ATTR uint8_t logical_wire(const i2slink_t *link, size_t u)
{
    return link->tx_logical[(u % N) ^ 1U];
}

/* Render tx_logical[a, a + length) (a and length even: memory and wire
 * ranges are then the same set) into tx_cycle, delayed by tx_shift bits.
 * Only that range's bits are written: the partial bytes at its ends are
 * merged, so a neighbour (H beside the report) never changes early. */
__attribute__((unused)) static IRAM_ATTR void tx_render(i2slink_t *link, size_t a, size_t length)
{
    const size_t s8 = link->tx_shift / 8U;
    const unsigned sb = link->tx_shift % 8U;
    const size_t count = length + (sb ? 1U : 0U);
    uint8_t *out = link->tx_cycle;
    const uint8_t *in = link->tx_logical;
    /* Physical wire byte q takes its bits from logical byte u = q - s8 (and,
     * with a partial shift, sb bits of the one before, MSB first). Both
     * indexes step by one and wrap by hand: no division in the loop (it
     * was three per byte, ~100 cycles a byte on the Sidecar). */
    size_t q = (a + s8) % N;
    size_t u = a % N;
    size_t before = u == 0U ? N - 1U : u - 1U;
    for (size_t index = 0U; index < count; ++index) {
        uint8_t v = in[u ^ 1U];
        uint8_t mask = 0xFFU;
        if (sb) {
            v = (uint8_t)((in[before ^ 1U] << (8U - sb)) | (v >> sb));
            if (index == 0U) {
                mask = (uint8_t)(0xFFU >> sb);
            } else if (index == count - 1U) {
                mask = (uint8_t)(0xFFU << (8U - sb));
            }
        }
        uint8_t *byte = &out[q ^ 1U];
        *byte = (uint8_t)((*byte & ~mask) | (v & mask));
        before = u;
        if (++u == N) {
            u = 0U;
        }
        if (++q == N) {
            q = 0U;
        }
    }
}

/* One word of the cycle (aligned offset), as the DMA should see it. */
static IRAM_ATTR void tx_store_word(i2slink_t *link, size_t offset, uint32_t value)
{
    if (TX_SYNC(link)) {
        memcpy(link->tx_logical + offset, &value, 4U);
        tx_render(link, offset, 4U);
    } else {
        *(volatile uint32_t *)(link->tx_cycle + offset) = value;
    }
}

/* The second report word: the torn-copy count (12 bits), a move this end
 * announces (8 bits: samples earlier; 2 bits: its number, 0 none), the
 * number of the peer's announcement it echoes (2 bits), and a check byte. */
static inline IRAM_ATTR uint32_t report2_encode(uint32_t torn, uint32_t move_d, uint32_t move_seq, uint32_t echo)
{
    const uint32_t v = (torn & 0xFFFU) | ((move_d & 0xFFU) << 12) | ((move_seq & 3U) << 20) | ((echo & 3U) << 22);
    const uint32_t check = 0x3CU ^ (v & 0xFFU) ^ ((v >> 8) & 0xFFU) ^ ((v >> 16) & 0xFFU);
    return v | (check << 24);
}

static inline IRAM_ATTR bool report2_decode(uint32_t v, uint32_t *torn, uint32_t *move_d, uint32_t *move_seq,
                                            uint32_t *echo)
{
    const uint32_t check = 0x3CU ^ (v & 0xFFU) ^ ((v >> 8) & 0xFFU) ^ ((v >> 16) & 0xFFU);
    if ((v >> 24) != check) {
        return false;
    }
    *torn = v & 0xFFFU;
    *move_d = (v >> 12) & 0xFFU;
    *move_seq = (v >> 20) & 3U;
    *echo = (v >> 22) & 3U;
    return true;
}

/* This end's second report word, as its state now says. */
static IRAM_ATTR void report2_store(i2slink_t *link)
{
    tx_store_word(link, OFF_REPORT2,
                  report2_encode(link->stats.torn, link->move_d, link->move_seq, link->echo_seq));
}

/* Move the peer's delay down by bits (mod the cycle): shift and re-render. */
__attribute__((unused)) static IRAM_ATTR void tx_shift_by(i2slink_t *link, uint32_t bits)
{
    link->tx_shift = (link->tx_shift + CYCLE_BITS - bits) % CYCLE_BITS;
    /* The whole cycle, at the new shift (with a partial byte the ends of
     * the one range meet in the same byte, merged in two steps). */
    tx_render(link, 0U, N);
    MEMW();
    ++link->stats.realigns;
    link->slot_proposal = 0U;   /* measured at the old place */
}

/* ---- Announced moves ----
 * A move of this end's frame (slot tuning) breaks the peer's lock: the
 * frame appears somewhere else in its ring, and a search on a ring the
 * re-render half rewrote was costing a command several ms. But the move is
 * known exactly: whole 16-bit samples earlier, so the peer's new lock is
 * its old one with the marker that many bytes earlier, same bit phase and
 * lane. So the mover announces it in its second report word (the move and
 * a number) and makes it only once the peer echoes the number in its own
 * (the peer may be idle, not reading, for any length of time; if no echo
 * comes in MOVE_ECHO_TIMEOUT_US, the move is dropped). An announcement
 * changes only the report word, not the version, so a peer that just polls
 * (hint() false: nothing new) sees it in the polls' periodic framing check,
 * which reads the reports from the live ring. The follower,
 * armed, doesn't need to know when the frame moves: when its framing
 * check fails at the old marker, it checks the new one (the same proof,
 * the VSYNC pulse and the HSYNC token there, in the live ring) and takes it
 * at once. The pass being re-rendered mixes the two, so for
 * FOLLOW_GRACE_CYCLES it keeps trying before dropping the lock. */
/* main is expected to poll the link every ~50 ms (from the bmon loop):
 * its echo can take that long, plus a cycle or two. */
#define MOVE_ECHO_TIMEOUT_US 250000U
#define FOLLOW_GRACE_CYCLES (3U * I2SLINK_CYCLE_US * CPU_MHZ)

#if I2SLINK_TX_AUTO_SYNC_CODE
/* Announce a move earlier by bits (a multiple of 16); made when echoed. */
static IRAM_ATTR void move_announce(i2slink_t *link, uint32_t bits)
{
    link->move_next_seq = (uint8_t)(link->move_next_seq % 3U + 1U);   /* 1, 2, 3, 1, ... */
    link->move_seq = link->move_next_seq;
    link->move_d = (uint8_t)(bits / 16U);
    link->move_since = esp_cpu_get_cycle_count();
    report2_store(link);
    ++link->stats.moves_announced;
}
#endif

/* ---- Slot tuning (tx_auto_sync; on by default) ----
 * A new version leaves when this end's TX DMA reaches H: its "slot", one
 * fixed instant per wire cycle, a fixed distance from the peer's (both
 * rings run on one BCLK). A fast response (written within SLOT_FAST_US of
 * the command's NEW read) is written at the same point of the cycle every
 * time, so its wait for the slot is a constant set at boot: anything up to
 * a cycle. Slot tuning measures that wait and moves the frame (tx_shift,
 * in whole 16-bit samples, so the peer's bit phase and lane stay 0) until
 * it is SLOT_MARGIN_US. Slower responses (SD I/O) land anywhere in the
 * cycle whatever the frame's place, so they are left out.
 *
 * The wait, without stopping: the polls (read, peek, hint) note the TX
 * DMA's descriptor (a register) and the CCOUNT whenever it changes; at a
 * fast write, the DMA's bit position is that boundary plus the cycles since,
 * less the register's lead (I2S_DMA_TX_DESC_LEAD descriptors: it runs ahead
 * of the data the DMA takes; uncorrected, the margin was ~56 us more than
 * meant), and the wait the distance from there to H: the last moment a
 * write still goes out in this pass. A boundary's uncertainty
 * is half the gap between the polls either side of it, growing with its age
 * (the two boards' clocks: SLOT_DRIFT_PPM): the most certain one is kept,
 * and a sample is skipped (stats.slot_unknown) when even that is off by
 * more than SLOT_REF_MAX_ERR_US.
 *
 * The responses aren't one point in the cycle: the reader takes a command
 * when its T passes, so a longer command arrives, and is answered, later
 * (variable-length frames), and the command mix sets the spread. So each
 * SLOT_WINDOW fast responses the frame goes where the window's total wait
 * would have been least, a response landing just after the slot counting
 * the whole cycle it waits, and one closer than SLOT_FLOOR_US before it
 * too (jitter would make it miss): every sample's wait brought to
 * SLOT_MARGIN_US is a candidate. Moves are made only for a clear gain
 * that two windows in a row agree on, within SLOT_AGREE_US (hysteresis):
 * the smaller of the two is made. A miss (a wait of more than half a cycle, once
 * tuned) is counted. */
/* Fast: answered within SLOT_FAST_US of the command's arrival. On the a24
 * bench (Sidecar at -O2, 2026-11-19) response times split with a gap:
 * RAM answers (Test chunks, status) under 256 us; SD-bound ones from 512 us
 * to tens of ms; between, only main's OTA install (Firmware chunks inflated
 * from SPIFFS, 256-512 us), left out. Default; i2slink_test_slot_config(). */
#define SLOT_FAST_US 256U
#define SLOT_MARGIN_US 25U
#define SLOT_FLOOR_US 12U   /* waits under it count as misses (slot_cost()) */
#define SLOT_TOLERANCE_US 8U
#define SLOT_AGREE_US 40U   /* two windows' proposals that agree (slot_sample()) */
#define SLOT_WINDOW 32U
#define SLOT_DRIFT_PPM 20U
#define SLOT_REF_MAX_ERR_US 8U
#define BITS_PER_US (32U * I2SLINK_FRAMES_PER_SECOND / 1000000U)
#define DESC_BITS (8U * DESC_BYTES)

#if I2SLINK_TX_AUTO_SYNC_CODE
/* The TX position reference's uncertainty now, in cycles. */
static inline IRAM_ATTR uint32_t tx_ref_error(const i2slink_t *link, uint32_t now)
{
    return link->tx_ref_err + (now - link->tx_ref_cc) / (1000000U / SLOT_DRIFT_PPM);
}

/* A poll's look at the TX DMA (tx_auto_sync only). */
static inline IRAM_ATTR void tx_ref_poll(i2slink_t *link)
{
    if (!TX_SYNC(link) || !link->slot_tune) {
        return;
    }
    const int d = i2s_dma_tx_desc_index(link->tx, link->tx_desc, I2SLINK_DESC_COUNT);
    const uint32_t now = esp_cpu_get_cycle_count();
    if (d < 0) {
        link->tx_seen_desc = -1;
        return;
    }
    if (d != link->tx_seen_desc) {
        const uint32_t gap = now - link->tx_seen_cc;
        if (link->tx_seen_desc >= 0 &&
            (unsigned)d == ((unsigned)link->tx_seen_desc + 1U) % I2SLINK_DESC_COUNT &&
            (!link->tx_ref_ok || gap / 2U <= tx_ref_error(link, now))) {
            link->tx_ref_desc = (uint8_t)d;
            link->tx_ref_cc = link->tx_seen_cc + gap / 2U;   /* the gap's middle */
            link->tx_ref_err = gap / 2U;
            link->tx_ref_ok = true;
        }
        link->tx_seen_desc = (int8_t)d;
    }
    link->tx_seen_cc = now;
}

static IRAM_ATTR void sort16(uint16_t *v, size_t n)
{
    for (size_t i = 1U; i < n; ++i) {
        const uint16_t x = v[i];
        size_t j = i;
        for (; j > 0U && v[j - 1U] > x; --j) {
            v[j] = v[j - 1U];
        }
        v[j] = x;
    }
}

/* One wait's cost in a placement: the wait, or, under SLOT_FLOOR_US (too
 * close: jitter would miss), a whole cycle more, as a miss. */
static inline IRAM_ATTR uint32_t slot_cost(uint32_t wait)
{
    return wait < SLOT_FLOOR_US * BITS_PER_US ? wait + CYCLE_BITS : wait;
}

/* A write just made: if a fast response, measure its wait; each window,
 * tune. */
static IRAM_ATTR void slot_sample(i2slink_t *link)
{
    const uint32_t now = esp_cpu_get_cycle_count();
    if (!link->slot_tune || !link->new_cc_ok || now - link->new_cc > link->slot_fast_cycles) {
        return;
    }
    const uint32_t since = now - link->tx_ref_cc;
    if (!link->tx_ref_ok || tx_ref_error(link, now) > SLOT_REF_MAX_ERR_US * CPU_MHZ ||
        since > 0x40000000U) {
        ++link->stats.slot_unknown;
        return;
    }
    /* Where the DMA really is: the register runs I2S_DMA_TX_DESC_LEAD
     * descriptors ahead of the data it sends. */
    const uint32_t pos = (uint32_t)((link->tx_ref_desc * DESC_BITS + CYCLE_BITS - I2S_DMA_TX_DESC_LEAD * DESC_BITS +
                                     (uint64_t)since * BITS_PER_US / CPU_MHZ) % CYCLE_BITS);
    const uint32_t h = (8U * OFF_H + link->tx_shift) % CYCLE_BITS;
    const uint32_t wait = (h + CYCLE_BITS - pos) % CYCLE_BITS;
    ++link->stats.slot_samples;
    if (link->stats.slot_tunes != 0U && wait > CYCLE_BITS / 2U) {
        ++link->stats.slot_misses;
    }
    link->slot_wait[link->slot_n] = (uint16_t)wait;
    link->slot_resp[link->slot_n] = (uint16_t)((now - link->new_cc) / CPU_MHZ);
    if (++link->slot_n < SLOT_WINDOW) {
        return;
    }
    link->slot_n = 0U;
    /* The move that minimizes the window's total wait (a response landing
     * just after the slot counts the lap it waits): try moving each
     * sample's wait to the margin. */
    const uint32_t margin = SLOT_MARGIN_US * BITS_PER_US;
    uint32_t stay = 0U, mean = 0U;
    for (size_t j = 0U; j < SLOT_WINDOW; ++j) {
        stay += slot_cost(link->slot_wait[j]);
        mean += link->slot_wait[j];
    }
    uint32_t best = stay, best_earlier = 0U;
    for (size_t i = 0U; i < SLOT_WINDOW; ++i) {
        const uint32_t earlier = ((link->slot_wait[i] + CYCLE_BITS - margin) % CYCLE_BITS) & ~15U;
        uint32_t cost = 0U;
        for (size_t j = 0U; j < SLOT_WINDOW; ++j) {
            cost += slot_cost((link->slot_wait[j] + CYCLE_BITS - earlier) % CYCLE_BITS);
        }
        if (cost < best) {
            best = cost;
            best_earlier = earlier;
        }
    }
    sort16(link->slot_wait, SLOT_WINDOW);
    sort16(link->slot_resp, SLOT_WINDOW);
    link->stats.slot_wait_lo = (uint16_t)(link->slot_wait[0] / BITS_PER_US);
    link->stats.slot_wait_mean = (uint16_t)(mean / SLOT_WINDOW / BITS_PER_US);
    link->stats.slot_wait_med = (uint16_t)(link->slot_wait[SLOT_WINDOW / 2U] / BITS_PER_US);
    link->stats.slot_wait_hi = (uint16_t)(link->slot_wait[SLOT_WINDOW - 1U] / BITS_PER_US);
    link->stats.slot_resp_med = link->slot_resp[SLOT_WINDOW / 2U];
    if (link->slot_trace)
    printf("SLOTTRACE t=%u shift %u waits lo %u med %u hi %u mean %u | best mean %u earlier %u | resp med %u hi %u\n",
           (unsigned)(now / CPU_MHZ / 1000U), (unsigned)link->tx_shift, link->slot_wait[0],
           link->slot_wait[SLOT_WINDOW / 2U], link->slot_wait[SLOT_WINDOW - 1U], (unsigned)(stay / SLOT_WINDOW),
           (unsigned)(best / SLOT_WINDOW), (unsigned)best_earlier, link->slot_resp[SLOT_WINDOW / 2U],
           link->slot_resp[SLOT_WINDOW - 1U]);
    /* Hysteresis: a move relocks the peer, so only for a clear gain (the
     * mean cost down by SLOT_TOLERANCE_US and by a fifth), and only when
     * two windows in a row propose it (within SLOT_TOLERANCE_US): one
     * window's jitter isn't a reason. */
    const uint32_t previous = link->slot_proposal;
    link->slot_proposal = 0U;
    if (link->sync_marker >= 0 || best_earlier == 0U ||
        stay - best < SLOT_WINDOW * SLOT_TOLERANCE_US * BITS_PER_US || 5U * best > 4U * stay) {
        return;   /* a fixed target (a test) owns the frame; or not worth a move */
    }
    /* Confirmed by a proposal within SLOT_AGREE_US: the two differ by the
     * windows' jitter (which sample is the slowest fast one). Make the
     * smaller move (the earlier is mod the cycle: the one behind): it keeps
     * both windows' waits at the margin or more. */
    const uint32_t apart = (best_earlier + CYCLE_BITS - previous) % CYCLE_BITS;
    const bool ahead = apart < CYCLE_BITS / 2U;   /* best_earlier moves further than previous */
    if (previous == 0U || (ahead ? apart : CYCLE_BITS - apart) > SLOT_AGREE_US * BITS_PER_US) {
        link->slot_proposal = (uint16_t)best_earlier;   /* propose; the next window confirms */
        return;
    }
    if (link->move_seq == 0U) {
        move_announce(link, ahead ? previous : best_earlier);
        ++link->stats.slot_tunes;
    }
}
#else
static inline void tx_ref_poll(i2slink_t *link) { (void)link; }
#endif

/* A peer's lock report, read from a checked copy or the live ring. */
static IRAM_ATTR void align_from_report(i2slink_t *link, uint32_t report, uint32_t report2)
{
    link->stats.peer_report = report;
    uint32_t torn = 0U, move_d = 0U, move_seq = 0U, echo = 0U;
    const bool torn_ok = report2_decode(report2, &torn, &move_d, &move_seq, &echo);
    if (torn_ok) {
        link->stats.peer_torn = torn;
        if (move_seq != 0U && move_seq != link->echo_seq) {
            /* The peer announces a move: arm it and say so. */
            link->echo_seq = (uint8_t)move_seq;
            link->follow_d = (uint8_t)move_d;
            link->follow_armed = true;
            link->follow_failing = false;
            report2_store(link);
        }
    }
    if (!TX_SYNC(link)) {
        return;
    }
#if I2SLINK_TX_AUTO_SYNC_CODE
    if (link->move_seq != 0U) {
        if (torn_ok && echo == link->move_seq) {
            /* Echoed: the peer follows the move. Make it. */
            const uint32_t bits = 16U * link->move_d;
            link->move_seq = 0U;
            report2_store(link);
            tx_shift_by(link, bits);
            ++link->stats.moves;
            return;
        }
        if (esp_cpu_get_cycle_count() - link->move_since > MOVE_ECHO_TIMEOUT_US * CPU_MHZ) {
            link->move_seq = 0U;   /* the peer isn't reading: leave the frame where it is */
            report2_store(link);
            ++link->stats.moves_dropped;
        }
    }
#endif
    uint32_t delay, gen;
    if (!report_decode(report, &delay, &gen) || delay >= CYCLE_BITS) {
        return;
    }
    if (gen != link->seen_gen) {
        link->seen_gen = (uint16_t)gen;
        link->seen_shift = link->tx_shift;
    } else if (link->tx_shift != link->seen_shift) {
        return;   /* measured at a shift this end has left since */
    }
    /* Only the bit phase and lane (the delay mod 16 bits): the decode needs
     * no more, and the frame's position in the cycle stays where it fell.
     * Moving it too (to D = 0) was tried in csim: the frame then passed
     * main's ring just as the Sidecar's responses were written, and most
     * of main's copies were torn (a wasted cycle each). */
    if (link->sync_marker < 0) {
        const uint32_t sub = delay % 16U;
        if (sub != 0U) {
            tx_shift_by(link, sub);
        }
        return;   /* phase and lane only; slot tuning places the frame */
    }
    /* A fixed target (i2slink_test_sync_marker()): the whole frame, the
     * peer's marker at sync_marker, phase 0, lane 0. */
    const uint32_t target = 8U * (((uint32_t)link->sync_marker + N - I2SLINK_VSYNC_BYTES) % N);
    const uint32_t err = (delay + CYCLE_BITS - target) % CYCLE_BITS;
    if (err != 0U) {
        tx_shift_by(link, err);
    }
}

/* The payload's first length bytes into stream positions [0, length) of
 * a cycle-layout buffer, zero-padded to the word (to L4). */
static IRAM_ATTR void stream_store(uint8_t *cycle, const uint8_t *payload, size_t length, size_t l4)
{
    size_t done = 0U;
    for (size_t run = 0U; run < RUN_COUNT && done < l4; ++run) {
        const size_t take = runs[run].length < l4 - done ? runs[run].length : l4 - done;
        const size_t data = done >= length ? 0U : (length - done < take ? length - done : take);
        memcpy(cycle + runs[run].offset, payload + done, data);
        memset(cycle + runs[run].offset + data, 0, take - data);
        done += take;
    }
}

/* The frame's CRC: the length word, then the payload. */
static inline IRAM_ATTR uint32_t frame_crc(uint32_t len_word, const uint8_t *payload, size_t length)
{
    return esp_rom_crc32_le(esp_rom_crc32_le(0U, (const uint8_t *)&len_word, 4U), payload, length);
}

IRAM_ATTR void i2slink_write(i2slink_t *link, const uint8_t *payload, size_t length)
{
    uint32_t sequence = link->tx_sequence + 1U;
    if (sequence == 0U) {
        sequence = 1U;
    }
    link->tx_sequence = sequence;
    if (length > I2SLINK_PAYLOAD_BYTES) {
        length = I2SLINK_PAYLOAD_BYTES;
    }
    const size_t l4 = (length + 3U) & ~3U;
    const size_t off_crc = stream_offset(l4);
    const size_t off_t = stream_offset(l4 + 4U);
    const uint32_t len_word = len_encode((uint32_t)length);
    const uint32_t crc = frame_crc(len_word, payload, length);
    if (TX_SYNC(link)) {
        /* The same order through the shift: the logical cycle first, then
         * rendered T, the payload and CRC, the length, H. */
        uint8_t *logical = link->tx_logical;
        memcpy(logical + off_t, &sequence, 4U);
        memcpy(logical + off_crc, &crc, 4U);
        memcpy(logical + OFF_LEN, &len_word, 4U);
        memcpy(logical + OFF_H, &sequence, 4U);
        stream_store(logical, payload, length, l4);
        tx_render(link, off_t, 4U);
        MEMW();
        size_t done = 0U;
        for (size_t run = 0U; run < RUN_COUNT && done < l4; ++run) {
            const size_t take = runs[run].length < l4 - done ? runs[run].length : l4 - done;
            tx_render(link, runs[run].offset, take);
            done += take;
        }
        tx_render(link, off_crc, 4U);
        tx_render(link, OFF_LEN, 4U);
        MEMW();
        tx_render(link, OFF_H, 4U);
        MEMW();
        ++link->stats.writes;
#if I2SLINK_TX_AUTO_SYNC_CODE
        slot_sample(link);
#endif
        return;
    }
    uint8_t *cycle = link->tx_cycle;

    /* T first (sampled last), payload and CRC, the length, then H (sampled
     * first). A pass that catches any mix shows H != T: an old length sends
     * the reader to the old T's place, a new one to the new T. */
    *(volatile uint32_t *)(cycle + off_t) = sequence;
    MEMW();
    stream_store(cycle, payload, length, l4);
    *(volatile uint32_t *)(cycle + off_crc) = crc;
    *(volatile uint32_t *)(cycle + OFF_LEN) = len_word;
    MEMW();
    *(volatile uint32_t *)(cycle + OFF_H) = sequence;
    MEMW();
    ++link->stats.writes;
}

IRAM_ATTR size_t i2slink_last_length(const i2slink_t *link)
{
    return link->last_length;
}

/* ------------------------------------------------------------------------ */
/* Tripwires                                                                */
/* ------------------------------------------------------------------------ */
/* The tripwires. G (the guard) is the last descriptor of the gap but one:
 * its length appears a whole spare descriptor before the beam reaches the
 * region. The front is the region descriptor holding the length word; E
 * the one holding T, which the length decides (variable-length frames). A
 * length means something only if it was cleared ("armed") after the beam's
 * previous crossing. The region's descriptors are armed together when G is
 * seen crossed (the beam is just entering the region), so every
 * write-back seen after that is this pass's:
 *   front clear, G clear: no crossing since arming
 *   front clear, G set:   the beam is entering the region: arm it, re-arm G
 *   front set,   G set:   both crossed since arming (we were late): re-arm
 *                         all, wait for the next pass
 *   front set,   G clear: the beam is in the region or past it: read this
 *                         pass's length (complete: the front has passed),
 *                         and E set means the window is open
 * A consumed window arms the region up to E (all behind the beam); the
 * rest, which a longer next frame could end in, is armed when G is seen.
 * Arming writes word 0 with length 0 (one store, no read-modify-write); the
 * DMA writes that word again only a lap later, and RX DMA ignores the
 * length. A store that lands just as the DMA writes back costs a window:
 * never a false one. */
typedef enum { TW_QUIET, TW_REGION, TW_WINDOW, TW_LATE } tripwire_t;

/* Arm descriptors from..to (inclusive, around the ring). */
static inline IRAM_ATTR void arm_range(i2slink_t *link, unsigned from, unsigned to)
{
    for (unsigned d = from;; d = (d + 1U) % I2SLINK_DESC_COUNT) {
        *link->dw0[d] = link->dw0_armed[d];
        if (d == to) {
            break;
        }
    }
}

/* The whole region and the guard (a new lock, or late). */
static inline IRAM_ATTR void arm_all(i2slink_t *link)
{
    arm_range(link, link->first_desc, link->max_end_desc);
    *link->guard_dw0 = link->guard_armed;
}

/* A window used: the region up to this pass's E. */
static inline IRAM_ATTR void arm_consumed(i2slink_t *link)
{
    arm_range(link, link->first_desc, link->cur_end_desc);
}

static inline IRAM_ATTR tripwire_t tripwire_step(i2slink_t *link)
{
    const uint32_t f = *link->dw0[link->front_desc] & LEN_MASK;   /* the front first, then G */
    const uint32_t g = *link->guard_dw0 & LEN_MASK;
    if (!f) {
        if (g) {
            arm_all(link);
            return TW_REGION;
        }
        return TW_QUIET;
    }
    if (g) {
        arm_all(link);
        return TW_LATE;
    }
    /* This pass's length, from the live ring, says which descriptor holds T. */
    MEMW();
    uint32_t length;
    link->cur_end_desc = len_decode(decode_u32(link, link->rx_ring, OFF_LEN), &length)
                             ? link->end_desc_of[(length + 3U) / 4U]
                             : link->max_end_desc;
    return (*link->dw0[link->cur_end_desc] & LEN_MASK) ? TW_WINDOW : TW_REGION;
}

/* H (the first stamp, written last) changed since the last clean copy: a
 * newer version has come. */
static inline IRAM_ATTR bool h_changed(const i2slink_t *link)
{
    return (((*link->h_word[0] ^ link->h_bits[0]) & link->h_mask[0]) |
            ((*link->h_word[1] ^ link->h_bits[1]) & link->h_mask[1])) != 0U;
}

/* Frame sync under the lock (wire_framing_ok()): VSYNC lies outside the
 * copied region, so this reads the live ring. */
static inline IRAM_ATTR bool framing_ok(const i2slink_t *link)
{
    MEMW();
    return wire_framing_ok(link->rx_ring, link->phase, link->lane, link->marker);
}

static void region_set(i2slink_t *link);   /* defined below, IRAM_ATTR there */

/* Framing failed at the lock. With a move armed (announced moves, above):
 * take the new lock if framing holds there, or keep the old one a while
 * (the re-render's pass). true: still locked. Else the lock is dropped. */
static IRAM_ATTR bool framing_lost(i2slink_t *link)
{
    if (link->follow_armed) {
        const uint16_t moved = (uint16_t)((link->marker + N - 2U * link->follow_d) % N);
        if (wire_framing_ok(link->rx_ring, link->phase, link->lane, moved)) {
            link->marker = moved;
            region_set(link);
            arm_all(link);
            link->follow_armed = false;
            link->follow_failing = false;
            ++link->stats.moves_followed;
            /* The new lock, for the peer's TX alignment: phase and lane as before. */
            tx_store_word(link, OFF_REPORT, report_encode(lock_delay(link), ++link->report_gen));
            return true;
        }
        const uint32_t now = esp_cpu_get_cycle_count();
        if (!link->follow_failing) {
            link->follow_failing = true;
            link->follow_fail_since = now;
            return true;
        }
        if (now - link->follow_fail_since < FOLLOW_GRACE_CYCLES) {
            return true;
        }
        link->follow_armed = false;   /* never showed up: search */
        link->follow_failing = false;
    }
    ++link->stats.framing;
    link->locked = false;
    return false;
}

SIDECAR_HOT_ATTR bool i2slink_hint(i2slink_t *link)
{
    tx_ref_poll(link);
    if (!link->locked) {
        ++link->stats.hints_true;
        return true;
    }
    const tripwire_t state = tripwire_step(link);
    if (state == TW_QUIET || state == TW_REGION) {
        sidecar_nez_delay(SIDECAR_POLL_DELAY);   /* callers poll: pace them */
        return false;                       /* no pass has ended since arming */
    }
    if (link->t_unread || h_changed(link)) {
        ++link->stats.hints_true;
        return true;                        /* tripwires left for read() */
    }
    /* H unchanged proves nothing about a stale lock: the bits under a wrong
     * lock's T can sit still (constant payload), or land on the armed
     * inverse, and then no read would ever come to check framing. So, at
     * most once per half cycle, check frame sync here too; broken, drop the
     * lock and say read (which searches). */
    const uint32_t now = esp_cpu_get_cycle_count();
    if (now - link->last_hint_framing >= PEEK_SEARCH_GAP_CYCLES) {
        link->last_hint_framing = now;
        if (!framing_ok(link)) {
            framing_lost(link);   /* a followed move, or the lock dropped: read either way */
            ++link->stats.hints_true;
            return true;
        }
        /* The reports change without a new version (an announced move,
         * the peer's lock): read them live, every end. */
        align_from_report(link, decode_u32(link, link->rx_ring, OFF_REPORT),
                          decode_u32(link, link->rx_ring, OFF_REPORT2));
    }
    if (state == TW_WINDOW) {
        arm_consumed(link);   /* same data again: this window is used */
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Reader                                                                   */
/* ------------------------------------------------------------------------ */
/* The raw ring bits of H's low byte, as two (word, mask) pairs (the second
 * mask is 0 when one word holds them all). */
static IRAM_ATTR void h_bits_set(i2slink_t *link)
{
    const size_t j = aligned_of(link, OFF_H);
    size_t byte_index[2] = {raw_high(link->lane, j), raw_low(link->lane, j)};
    uint8_t byte_mask[2] = {(uint8_t)(0xFFU >> link->phase),
                            (uint8_t)(link->phase ? (0xFFU << (8U - link->phase)) & 0xFFU : 0U)};
    const volatile uint32_t *ring32 = (const volatile uint32_t *)link->rx_ring;
    link->h_word[0] = &ring32[byte_index[0] / 4U];
    link->h_mask[0] = (uint32_t)byte_mask[0] << (8U * (byte_index[0] % 4U));
    link->h_word[1] = &ring32[byte_index[1] / 4U];
    link->h_mask[1] = (uint32_t)byte_mask[1] << (8U * (byte_index[1] % 4U));
    if (link->h_word[1] == link->h_word[0]) {
        link->h_mask[0] |= link->h_mask[1];
        link->h_mask[1] = 0U;
    }
}

/* Remember H's raw bits (read from the ring just before H was decoded)
 * for the version just verified, quieting hint(). */
static IRAM_ATTR void h_bits_save(i2slink_t *link, const uint32_t h_raw[2])
{
    link->h_bits[0] = h_raw[0];
    link->h_bits[1] = h_raw[1];
    link->t_unread = false;   /* a clean copy: from here on, H tells */
}

/* The RX descriptor holding the raw bytes of aligned TX offset end - 1
 * (end exclusive) under the lock. */
static IRAM_ATTR unsigned desc_of_end(const i2slink_t *link, size_t end)
{
    const long e = (long)link->marker + (long)end - (long)I2SLINK_VSYNC_BYTES;
    const long wire_last = (e + 1L + (long)link->lane) | 1L;
    return (unsigned)((((wire_last % (long)N) + (long)N) % (long)N) / DESC_BYTES);
}

static IRAM_ATTR void region_set(i2slink_t *link)
{
    /* Raw range for aligned [first, end): wire [first - 1 + lane,
     * end + 1 + lane], rounded out to whole sample pairs (wire and memory
     * differ by ^ 1 within a pair). */
    const long first = (long)link->marker + (long)(OFF_H - I2SLINK_VSYNC_BYTES);
    const long end = (long)link->marker + (long)(OFF_END - I2SLINK_VSYNC_BYTES);
    const long wire_first = (first - 1L + (long)link->lane) & ~1L;
    const long wire_last = (end + 1L + (long)link->lane) | 1L;
    const size_t start = (size_t)(((wire_first % (long)N) + (long)N) % (long)N);
    const size_t length = (size_t)(wire_last - wire_first + 1L);
    link->region_start = (uint16_t)start;
    link->region_length = (uint16_t)length;
    const unsigned first_desc = (unsigned)(start / DESC_BYTES);
    const unsigned end_desc = (unsigned)(((start + length - 1U) % N) / DESC_BYTES);
    /* Two before the region's first descriptor: a whole spare descriptor
     * separates the guard's write-back from the region (static assert). */
    const unsigned guard_desc = (first_desc + I2SLINK_DESC_COUNT - 2U) % I2SLINK_DESC_COUNT;
    link->first_desc = (uint8_t)first_desc;
    link->max_end_desc = (uint8_t)end_desc;
    link->cur_end_desc = (uint8_t)end_desc;
    link->front_desc = (uint8_t)desc_of_end(link, OFF_LEN + 4U);
    for (size_t w = 0U; w < STREAM_WORDS; ++w) {
        link->end_desc_of[w] = (uint8_t)desc_of_end(link, stream_offset(4U * w + 4U) + 4U);
    }
    link->guard_dw0 = link->dw0[guard_desc];
    link->guard_armed = link->dw0_armed[guard_desc];
    h_bits_set(link);
    /* hint() is true until the first clean copy, so a version already
     * waiting is not stranded. A flag, not an inverted snapshot of the T
     * bits: the ring can change before that copy (a torn pass completing),
     * and if its new T bits matched the inverse, hint() would say
     * "unchanged" for ever with an unread frame there, while the peer,
     * waiting for its ack, never writes again: the link deadlocks. */
    link->t_unread = true;
}

/* For simulation only (nothing on the chip): a guess at the general
 * decode's cost per byte (two loads, a shift and merge, index math). */
#define DECODE_SIM_CYCLES_PER_BYTE 12U

/* One pass over the region, straight from the live RX ring (no copy of the
 * ring): H, then the payload decoded into the caller's buffer, the CRC
 * word, and T, in wire order, so a beam that overtakes the decode leaves T
 * newer than H (torn). A stale H (the version last returned, or 0) skips
 * the payload: the caller's buffer is touched only for a version that may
 * be new. */
typedef struct {
    uint32_t h, t, crc, report, report2, len_word;
    uint32_t length;     /* the pass's payload length (the most when its length word is bad) */
    uint32_t h_raw[2];   /* H's raw ring bits, read just before H */
    bool payload_read;
} pass_t;

static IRAM_ATTR void region_pass(i2slink_t *link, uint8_t *payload, pass_t *pass)
{
    const uint8_t *raw = link->rx_ring;
    MEMW();
    /* H's raw bits before H itself: if they change in between, the saved
     * bits are the older ones and hint() just says look again (saved
     * after, a newer H's bits could hide an unread version). */
    pass->h_raw[0] = *link->h_word[0];
    pass->h_raw[1] = *link->h_word[1];
    MEMW();
    pass->h = decode_u32(link, raw, OFF_H);
    pass->report = decode_u32(link, raw, OFF_REPORT);
    pass->report2 = decode_u32(link, raw, OFF_REPORT2);
    pass->len_word = decode_u32(link, raw, OFF_LEN);
    if (!len_decode(pass->len_word, &pass->length)) {
        pass->length = I2SLINK_PAYLOAD_BYTES;   /* then H != T or the CRC fails */
    }
    const size_t l4 = (pass->length + 3U) & ~3U;
    pass->payload_read = !(pass->h == 0U || (link->have_read && pass->h == link->last_read));
    if (pass->payload_read) {
        uint8_t *out = payload;
        size_t done = 0U;
        for (size_t run = 0U; run < RUN_COUNT && done < l4; ++run) {
            const size_t take = runs[run].length < l4 - done ? runs[run].length : l4 - done;
            wire_decode_run_n(raw, link->phase, link->lane, link->marker, run, out, take);
            out += take;
            done += take;
            /* For simulation only: the decode takes time, and the DMA keeps
             * writing the ring meanwhile. */
            SIDECAR_SIM_ELAPSE(take * DECODE_SIM_CYCLES_PER_BYTE);
        }
    }
    pass->crc = decode_u32(link, raw, stream_offset(l4));
    MEMW();
    pass->t = decode_u32(link, raw, stream_offset(l4 + 4U));
}


/* Search one snapshot of the ring at all 16 bit offsets, starting from the
 * last lock's. */
/* allow_full: read()'s searches also try the exhaustive search when the
 * anchored one fails (peek() stays anchored-only, to fail fast). */
static IRAM_ATTR bool search(i2slink_t *link, bool allow_full)
{
    MEMW();
    memcpy(link->search_raw, link->rx_ring, N);
    uint8_t phase, lane;
    uint16_t marker;
    bool fallback = false;
    const bool found = wire_search(link->search_raw, link->search_aligned, link->rx_tag, link->phase,
                                   link->lane, &phase, &lane, &marker, allow_full, &fallback);
    /* For simulation only (nothing on the chip): the search's CPU time. */
    SIDECAR_SIM_CPU_US(allow_full && (!found || fallback) ? SIDECAR_SEARCH_US_ANCHORED + SIDECAR_SEARCH_US_FULL
                                                          : SIDECAR_SEARCH_US_ANCHORED);
    if (!found) {
        return false;
    }
    if (fallback) {
        ++link->stats.locks_fallback;
    }
    link->phase = phase;
    link->lane = lane;
    link->marker = marker;
    region_set(link);
    arm_all(link);
    link->locked = true;
    link->follow_armed = false;   /* a found lock: any announced move is in it already */
    link->follow_failing = false;
    ++link->stats.locks;
    /* Tell the peer (an tx_auto_sync peer shifts its TX to make this 0). */
    tx_store_word(link, OFF_REPORT, report_encode(lock_delay(link), ++link->report_gen));
    MEMW();
    return true;
}

static inline IRAM_ATTR i2slink_status_t finish(i2slink_t *link, i2slink_status_t status)
{
    if (TX_SYNC(link) && status == I2SLINK_NEW) {
        link->new_cc = esp_cpu_get_cycle_count();   /* slot tuning: a command's arrival */
        link->new_cc_ok = true;
    }
    switch (status) {
    case I2SLINK_NEW: ++link->stats.reads_new; break;
    case I2SLINK_TIMEOUT: ++link->stats.reads_timeout; break;
    case I2SLINK_TORN: ++link->stats.reads_torn; break;
    case I2SLINK_CRC: ++link->stats.reads_crc; break;
    case I2SLINK_NO_LOCK: ++link->stats.reads_no_lock; break;
    case I2SLINK_STALLED: ++link->stats.reads_stalled; break;
    }
    return status;
}

/* One beam window's copy, checked. */
typedef enum { COPY_NONE, COPY_NEW, COPY_STALE, COPY_TORN, COPY_CRC, COPY_FRAMING } copy_t;

/* Check one region pass (framing, the H and T stamps, the CRC). On NEW the
 * caller's buffer holds the version; on a torn or CRC-failing pass it may
 * hold anything; on a stale one it is untouched. Shared by take_window()
 * and peek(). */
static IRAM_ATTR copy_t check_pass(i2slink_t *link, const uint8_t *payload, const pass_t *pass,
                                   uint32_t *h_out)
{
    if (!framing_ok(link)) {
        return framing_lost(link) ? COPY_NONE : COPY_FRAMING;   /* followed (or following) a move: again */
    }
    ++link->stats.windows;
    if (wire_is_aligned(link->phase, link->lane)) {
        ++link->stats.fast_copies;
    }
    /* Every end: the peer's reports (a follower arms announced moves). */
    align_from_report(link, pass->report, pass->report2);
    const uint32_t h = pass->h;
    if (h != pass->t) {
        ++link->stats.torn;
        /* Tell the peer (a tx_auto_sync peer hunts for a frame position
         * that doesn't tear). */
        report2_store(link);
        return COPY_TORN;
    }
    if (!pass->payload_read) {
        ++link->stats.stale;
        h_bits_save(link, pass->h_raw);
        return COPY_STALE;
    }
    if (frame_crc(pass->len_word, payload, pass->length) != pass->crc) {
        ++link->stats.crc_errors;
        return COPY_CRC;
    }
    if (link->have_read) {
        const uint32_t step = h - link->last_read;
        if (step > (1U << 24)) {
            ++link->stats.jumps;           /* a peer restart (random start) */
        } else if (step > 1U) {
            link->stats.skipped += step - 1U;
        }
    }
    link->last_read = h;
    link->have_read = true;
    link->last_length = (uint16_t)pass->length;
    h_bits_save(link, pass->h_raw);
    *h_out = h;
    return COPY_NEW;
}

/* The guard is checked as the copy starts, not after it: the window (E to
 * G) shrinks as the frame grows (a full frame leaves about one descriptor
 * of wire time; a short one most of the cycle), and at the largest frames
 * (I2SLINK_LINE_COUNT) the copy outlasts it, so a check after it failed
 * every window. Starting inside the window is enough: the copy runs from
 * H, the region's first word, in wire order and far faster than the wire,
 * so the beam coming back into the region can't overtake it. If it ever
 * did (a stalled copy), T, copied last, would be the newer cycle's and
 * check_pass() sees H != T (torn), with the CRC behind it. */
static IRAM_ATTR copy_t take_window(i2slink_t *link, uint8_t *payload, uint32_t *h_out)
{
    pass_t pass;
    const unsigned irq = portSET_INTERRUPT_MASK_FROM_ISR();
    MEMW();
    const bool guard_crossed = (*link->guard_dw0 & LEN_MASK) != 0U;
    if (!guard_crossed) {
        region_pass(link, payload, &pass);
        MEMW();
    }
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    arm_consumed(link);                     /* consume this window */
    if (guard_crossed) {
        ++link->stats.tripped;
        arm_all(link);
        return COPY_NONE;
    }
    return check_pass(link, payload, &pass, h_out);
}

SIDECAR_HOT_ATTR i2slink_status_t i2slink_read(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                                               uint32_t *sequence, uint32_t wait_us)
{
    const bool forever = wait_us == I2SLINK_WAIT_FOREVER;
    const uint64_t wait_cycles = (uint64_t)wait_us * CPU_MHZ;
    uint64_t elapsed = 0U;            /* since the call */
    uint64_t fault_since = 0U;        /* elapsed at the last clean copy (or the call) */
    bool clean = false;               /* a clean copy seen in this call */
    i2slink_status_t fault = I2SLINK_STALLED;   /* until the beam is seen moving */
    bool first = true;
    uint32_t last = esp_cpu_get_cycle_count();
    for (;;) {
        tx_ref_poll(link);
        if (!link->locked) {
            first = false;
            if (!search(link, true)) {
                fault = I2SLINK_NO_LOCK;
                /* A failed search decodes the snapshot at all 16 offsets,
                 * a long stretch of SRAM traffic; with no lock there is no
                 * window to miss, so rest before the next snapshot. */
                sidecar_nez_delay(SEARCH_BACKOFF_ITERATIONS);
            }
        } else {
            const tripwire_t state = tripwire_step(link);
            if (first) {
                first = false;
                if (state == TW_WINDOW) {
                    ++link->stats.immediate;
                } else {
                    ++link->stats.waited;
                }
            }
            if (state != TW_QUIET && fault == I2SLINK_STALLED) {
                fault = I2SLINK_TORN;    /* the DMA is moving */
            }
            if (state == TW_LATE) {
                ++link->stats.late;
            } else if (state == TW_WINDOW) {
                uint32_t h = 0U;
                switch (take_window(link, payload, &h)) {
                case COPY_NEW:
                    if (sequence) {
                        *sequence = h;
                    }
                    return finish(link, I2SLINK_NEW);
                case COPY_STALE:
                    clean = true;
                    fault_since = elapsed;
                    fault = I2SLINK_TORN;
                    break;
                case COPY_TORN: fault = I2SLINK_TORN; break;
                case COPY_CRC: fault = I2SLINK_CRC; break;
                case COPY_FRAMING: fault = I2SLINK_NO_LOCK; break;
                case COPY_NONE: break;
                }
            }
        }
        const uint32_t now = esp_cpu_get_cycle_count();
        elapsed += now - last;
        last = now;
        if (clean && !forever && elapsed >= wait_cycles) {
            return finish(link, I2SLINK_TIMEOUT);
        }
        if (elapsed - fault_since > link->sync_timeout_cycles) {
            return finish(link, fault);
        }
        /* Pace the poll: spare the SRAM banks the core 1 bus loop shares.
         * take_window() runs in the same pass that sees TW_WINDOW, so this
         * never sits between a window and its copy; it can only make the
         * next look up to one delay late, a small slice of the window. */
        sidecar_nez_delay(SIDECAR_POLL_DELAY);
    }
}



SIDECAR_HOT_ATTR i2slink_status_t i2slink_peek(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                                               uint32_t *sequence)
{
    ++link->stats.peeks;
    tx_ref_poll(link);
    if (!link->locked) {
        const uint32_t now = esp_cpu_get_cycle_count();
        if (link->peek_searched && now - link->last_peek_search < PEEK_SEARCH_GAP_CYCLES) {
            return I2SLINK_NO_LOCK;
        }
        link->peek_searched = true;
        link->last_peek_search = now;
        if (!search(link, false)) {
            return finish(link, I2SLINK_NO_LOCK);
        }
        /* Locked: a complete version may already be in the ring (region_set()
         * leaves h_changed() true until the first read). */
    }
    /* H is the last field the writer stores: unchanged, nothing new has
     * come. But a lock can go stale with H never changing: the peer
     * restarting its clock can bring the stream back at another bit offset,
     * and then the old H position just holds bits that don't move. So now
     * and then (at most once per half cycle) check frame sync (framing_ok():
     * the VSYNC pulse and the HSYNC token after it, in the live ring). Broken
     * framing drops the lock; the next peek searches. A changed H may still
     * be arriving (its T not yet in): that copy fails H == T and the next
     * peek retries. */
    if (!link->t_unread && !h_changed(link)) {
        const uint32_t now = esp_cpu_get_cycle_count();
        if (now - link->last_peek_framing >= PEEK_SEARCH_GAP_CYCLES) {
            link->last_peek_framing = now;
            if (!framing_ok(link)) {
                return framing_lost(link) ? I2SLINK_TIMEOUT : finish(link, I2SLINK_NO_LOCK);
            }
            align_from_report(link, decode_u32(link, link->rx_ring, OFF_REPORT),
                              decode_u32(link, link->rx_ring, OFF_REPORT2));   /* every end, as in hint() */
        }
        return I2SLINK_TIMEOUT;
    }
    /* Copy now, no window. A copy the DMA tears (a newer version starting
     * mid-copy) has H != T, or fails the CRC: then the next peek retries. */
    pass_t pass;
    const unsigned irq = portSET_INTERRUPT_MASK_FROM_ISR();
    region_pass(link, payload, &pass);
    MEMW();
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    uint32_t h = 0U;
    switch (check_pass(link, payload, &pass, &h)) {
    case COPY_NEW:
        if (sequence) {
            *sequence = h;
        }
        return finish(link, I2SLINK_NEW);
    case COPY_FRAMING:
        return finish(link, I2SLINK_NO_LOCK);
    case COPY_TORN:
    case COPY_CRC:
        ++link->stats.peeks_torn;
        return I2SLINK_TIMEOUT;
    default:
        return I2SLINK_TIMEOUT;   /* stale: what was last returned */
    }
}

bool i2slink_test_lock(const i2slink_t *link, uint8_t *phase, uint8_t *lane, uint16_t *marker)
{
    *phase = link->phase;
    *lane = link->lane;
    *marker = link->marker;
    return link->locked;
}

void i2slink_test_break_lock(i2slink_t *link, uint32_t seed, bool keep_locked)
{
    if (!link->locked) {
        return;
    }
    uint32_t r = seed | 1U;
    unsigned phase, lane, marker;
    do {
        r = r * 1103515245U + 12345U;
        phase = (r >> 8) & 7U;
        lane = (r >> 11) & 1U;
        marker = (r >> 12) % N;
    } while (phase == link->phase && lane == link->lane && marker == link->marker);
    link->phase = (uint8_t)phase;
    link->lane = (uint8_t)lane;
    link->marker = (uint16_t)marker;
    region_set(link);
    arm_all(link);
    link->locked = keep_locked;
}

void i2slink_test_shift_tx(i2slink_t *link, uint32_t seed)
{
    if (!TX_SYNC(link)) {
        return;
    }
    uint32_t r = seed * 2654435761U + 0x9E3779B9U, shift;
    do {
        r = r * 1103515245U + 12345U;
        shift = (r >> 8) % CYCLE_BITS;
    } while (shift == link->tx_shift);
    /* A report not yet seen was measured before the scramble. */
    uint32_t delay, gen;
    if (report_decode(link->stats.peer_report, &delay, &gen) && gen != link->seen_gen) {
        link->seen_gen = (uint16_t)gen;
        link->seen_shift = link->tx_shift;
    }
    link->tx_shift = shift;
    tx_render(link, 0U, N);
    MEMW();
}

void i2slink_test_sync_marker(i2slink_t *link, int marker)
{
    link->sync_marker = marker < 0 ? -1 : (int32_t)((unsigned)marker % N);
}

void i2slink_test_slot_tune(i2slink_t *link, bool on)
{
    link->slot_tune = on && TX_SYNC(link);
    link->slot_n = 0U;
    link->tx_ref_ok = false;
    link->tx_seen_desc = -1;
}

void i2slink_test_slot_config(i2slink_t *link, uint32_t fast_us, bool trace)
{
    link->slot_fast_cycles = fast_us * CPU_MHZ;
    link->slot_trace = trace;
}

bool i2slink_report_decode(uint32_t report, uint32_t *delay, uint32_t *gen)
{
    return report_decode(report, delay, gen);
}

void i2slink_test_latency(i2slink_t *link, uint32_t n, i2slink_latency_t *out)
{
    memset(out, 0, sizeof(*out));
    out->slot_min = out->pipe_min = UINT32_MAX;
    uint64_t slot_sum = 0U, pipe_sum = 0U, fifo_sum[3] = {0U, 0U, 0U}, rx_sum[3] = {0U, 0U, 0U};
    uint8_t frame[16], back[I2SLINK_PAYLOAD_BYTES];
    /* The slot: the register enters the descriptor I2S_DMA_TX_DESC_LEAD past H's. */
    const unsigned h_desc =
        (unsigned)((((OFF_H + link->tx_shift / 8U) % N) / DESC_BYTES + I2S_DMA_TX_DESC_LEAD) % I2SLINK_DESC_COUNT);
    for (uint32_t i = 0U; i < n; ++i) {
        esp_rom_delay_us(esp_random() % 500U);
        memset(frame, 0, sizeof(frame));
        memcpy(frame, &i, sizeof(i));
        frame[4] = 0xA5U;
        const uint32_t t_w = esp_cpu_get_cycle_count();
        i2slink_write(link, frame, sizeof(frame));
        /* The slot: the TX DMA moves into H's descriptor. */
        uint32_t t_s = t_w;
        int d = i2s_dma_tx_desc_index(link->tx, link->tx_desc, I2SLINK_DESC_COUNT);
        for (;;) {
            const int now = i2s_dma_tx_desc_index(link->tx, link->tx_desc, I2SLINK_DESC_COUNT);
            t_s = esp_cpu_get_cycle_count();
            if (now == (int)h_desc && d != (int)h_desc) {
                uint32_t bytes[3];
                i2s_dma_tx_fifo_bytes(link->tx, bytes);
                for (unsigned level = 0U; level < 3U; ++level) {
                    fifo_sum[level] += bytes[level];
                    out->fifo_max[level] = bytes[level] > out->fifo_max[level] ? bytes[level] : out->fifo_max[level];
                }
                break;
            }
            d = now;
            if (t_s - t_w > 2U * I2SLINK_CYCLE_US * CPU_MHZ) {
                break;   /* no register (csim): leave it at this */
            }
        }
        bool got = false;
        uint32_t t_r = t_s;
        while (esp_cpu_get_cycle_count() - t_w < 5000U * CPU_MHZ) {
            if (i2slink_read(link, back, NULL, 2000U) == I2SLINK_NEW && memcmp(back, frame, 5U) == 0) {
                t_r = esp_cpu_get_cycle_count();
                got = true;
                uint32_t bytes[3];
                i2s_dma_rx_fifo_bytes(link->rx, bytes);
                for (unsigned level = 0U; level < 3U; ++level) {
                    rx_sum[level] += bytes[level];
                    out->rx_fifo_max[level] =
                        bytes[level] > out->rx_fifo_max[level] ? bytes[level] : out->rx_fifo_max[level];
                }
                break;
            }
        }
        if (!got) {
            ++out->failed;
            continue;
        }
        const uint32_t slot = (t_s - t_w) / CPU_MHZ, pipe = (t_r - t_s) / CPU_MHZ;
        ++out->n;
        slot_sum += slot;
        pipe_sum += pipe;
        out->slot_min = slot < out->slot_min ? slot : out->slot_min;
        out->slot_max = slot > out->slot_max ? slot : out->slot_max;
        out->pipe_min = pipe < out->pipe_min ? pipe : out->pipe_min;
        out->pipe_max = pipe > out->pipe_max ? pipe : out->pipe_max;
    }
    if (out->n) {
        out->slot_mean = (uint32_t)(slot_sum / out->n);
        out->pipe_mean = (uint32_t)(pipe_sum / out->n);
        for (unsigned level = 0U; level < 3U; ++level) {
            out->fifo_mean[level] = (uint32_t)(fifo_sum[level] / n);
            out->rx_fifo_mean[level] = (uint32_t)(rx_sum[level] / out->n);
        }
    }
}

uint32_t i2slink_test_tx_before_slot(i2slink_t *link, unsigned descs_before)
{
    const size_t h_byte = (OFF_H + link->tx_shift / 8U) % N;
    const unsigned h_desc = (unsigned)(h_byte / DESC_BYTES);
    const unsigned target = (h_desc + I2S_DMA_TX_DESC_LEAD + I2SLINK_DESC_COUNT - descs_before % I2SLINK_DESC_COUNT) %
                            I2SLINK_DESC_COUNT;
    const uint32_t start = esp_cpu_get_cycle_count();
    int d = i2s_dma_tx_desc_index(link->tx, link->tx_desc, I2SLINK_DESC_COUNT);
    for (;;) {
        const int now = i2s_dma_tx_desc_index(link->tx, link->tx_desc, I2SLINK_DESC_COUNT);
        const uint32_t t = esp_cpu_get_cycle_count();
        if (now == (int)target && d != (int)target) {
            /* The DMA reaches H descs_before descriptors and H's offset in its own later. */
            const uint32_t bytes = descs_before * DESC_BYTES + (uint32_t)(h_byte % DESC_BYTES);
            return t + (uint32_t)((uint64_t)bytes * 8U * CPU_MHZ / BITS_PER_US);
        }
        d = now;
        if (t - start > 2U * I2SLINK_CYCLE_US * CPU_MHZ) {
            return 0U;
        }
    }
}


void i2slink_test_redeliver(i2slink_t *link)
{
    link->have_read = false;   /* the current version counts as unread */
    link->t_unread = true;
}

static void dump_bytes(const char *name, const char *what, const uint8_t *bytes)
{
    char line[3 * 32 + 1];
    for (size_t row = 0U; row < N; row += 32U) {
        size_t used = 0U;
        for (size_t col = 0U; col < 32U && row + col < N; ++col) {
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%02x ", bytes[row + col]);
        }
        LOG("%s %s %03u %s\n", name, what, (unsigned)row, line);
    }
}

void i2slink_test_dump(const i2slink_t *link, const char *name)
{
    static uint8_t snap[N];
    MEMW();
    memcpy(snap, link->rx_ring, N);   /* one copy, so the rows agree */
    dump_bytes(name, "ring now", snap);
    dump_bytes(name, "last search snapshot", link->search_raw);
    LOG("%s lock: %s phase %u lane %u marker %u, region %u+%u\n", name,
           link->locked ? "locked" : "unlocked", link->phase, link->lane, link->marker,
           link->region_start, link->region_length);
}

void i2slink_debug_dump(const i2slink_t *link, const char *name)
{
    char line[3 * 32 + 1];
    for (size_t row = 0U; row < N; row += 32U) {
        size_t used = 0U;
        for (size_t col = 0U; col < 32U && row + col < N; ++col) {
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%02x ",
                                     link->rx_ring[row + col]);
        }
        ESP_LOGI(TAG, "%s ring %03u %s", name, (unsigned)row, line);
    }
    ESP_LOGI(TAG, "%s dw0[0..3] %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32, name,
             *link->dw0[0], *link->dw0[1], *link->dw0[2], *link->dw0[3]);
}

void i2slink_memory(const i2slink_t *link, uintptr_t *start, size_t *size)
{
    *start = (uintptr_t)link;
    *size = link ? sizeof(*link) : 0U;
}

void i2slink_static_memory(uintptr_t *start, size_t *size)
{
    const struct { const void *at; size_t size; } tables[] = {
        {hsync, sizeof(hsync)},
        {runs, sizeof(runs)},
        {TAG_SOURCE, sizeof(TAG_SOURCE)},
        {TAG_FOLLOWER, sizeof(TAG_FOLLOWER)},
    };
    uintptr_t low = UINTPTR_MAX, high = 0U;
    for (size_t index = 0U; index < sizeof(tables) / sizeof(tables[0]); ++index) {
        const uintptr_t at = (uintptr_t)tables[index].at;
        low = at < low ? at : low;
        high = at + tables[index].size > high ? at + tables[index].size : high;
    }
    *start = low;
    *size = high - low;
}

void i2slink_stats(const i2slink_t *link, i2slink_stats_t *stats)
{
    *stats = link->stats;
    stats->locked = link->locked;
    stats->bit_phase = link->phase;
    stats->byte_lane = link->lane;
    stats->marker_offset = link->marker;
    stats->tx_shift = link->tx_shift;
    stats->sync_target = link->sync_marker;
    stats->slot_tune = link->slot_tune;
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                    */
/* ------------------------------------------------------------------------ */
#define RETURN_ON_ERROR(expression)          \
    do {                                     \
        esp_err_t err_ = (expression);       \
        if (err_ != ESP_OK) return err_;     \
    } while (0)

static i2s_std_config_t std_config(const i2slink_config_t *config, int dout, int din)
{
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2SLINK_FRAMES_PER_SECOND),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_gpio,
            .ws = I2S_GPIO_UNUSED,
            .dout = dout,
            .din = din,
        },
    };
    std.clk_cfg.clk_src = CLOCK_SOURCE;
    if (config->clock == I2SLINK_CLOCK_FOLLOWER) {
        /* Follower (all-slave) channels: the module clock the bench used. */
        std.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t)96;
    }
    return std;
}

static i2s_chan_config_t chan_config(int port, i2s_role_t role)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(port, role);
    chan.dma_desc_num = I2SLINK_DESC_COUNT;
    chan.dma_frame_num = 256U;   /* driver buffers (freed by the rings) >= DESC_BYTES */
    return chan;
}

/* Two simplex channels on the configured I2S. With no WS pin IDF does not join them into
 * full duplex (which runs the second channel as an internal slave and lost
 * ~8% of its data on the bench), so each keeps its own clocking: the
 * source's TX is a master driving BCLK, every other channel a slave
 * sampling BCLK from the pad. */
static esp_err_t channels_create(i2slink_t *link, const i2slink_config_t *config)
{
    const bool source = config->clock == I2SLINK_CLOCK_SOURCE;
    i2s_chan_config_t chan = chan_config(config->i2s_port, I2S_ROLE_SLAVE);
    RETURN_ON_ERROR(i2s_new_channel(&chan, NULL, &link->rx));
    i2s_std_config_t std = std_config(config, I2S_GPIO_UNUSED, config->rx_gpio);
    std.gpio_cfg.invert_flags.bclk_inv = I2SLINK_RX_BCLK_INVERT;
    RETURN_ON_ERROR(i2s_channel_init_std_mode(link->rx, &std));
    chan = chan_config(config->i2s_port, source ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
    RETURN_ON_ERROR(i2s_new_channel(&chan, &link->tx, NULL));
    std = std_config(config, config->tx_gpio, I2S_GPIO_UNUSED);
    RETURN_ON_ERROR(i2s_channel_init_std_mode(link->tx, &std));
    /* The pad is shared: keep its input enabled for RX (the source's TX
     * drives it as an output). */
    RETURN_ON_ERROR(gpio_input_enable(config->bclk_gpio));
    /* Loopback on one pad: the same for the data pin, which RX set up as an
     * input before TX took it as an output. */
    if (config->loopback && config->rx_gpio == config->tx_gpio) {
        RETURN_ON_ERROR(gpio_input_enable(config->tx_gpio));
    }
    return ESP_OK;
}

/* The 32 KB internal SRAM bank holding a data address, numbered as in
 * tools/verify_iram_hotspots.py (bank 0 at IRAM 0x40370000, which DRAM
 * 0x3FC80000 would alias); -1 outside internal SRAM. */
static int sram_bank(uintptr_t address)
{
    if (address < 0x3FC88000U || address >= 0x3FD00000U) {
        return -1;
    }
    return (int)((address - 0x3FC80000U) / 0x8000U);
}

/* Where the link's DMA memory landed, to compare against the banks the
 * core 1 bus loop uses: the poll reads the RX descriptors, the DMA writes
 * them and the RX ring and reads the TX descriptors and cycle. */
static void log_placement(const i2slink_t *link)
{
    const uintptr_t start = (uintptr_t)link, end = start + sizeof(*link);
    const uintptr_t ring = (uintptr_t)link->rx_ring;
    uintptr_t rx_first = 0U, rx_end = 0U, tx_first = 0U, tx_end = 0U;
    i2s_dma_desc_span(link->rx, &rx_first, &rx_end);
    i2s_dma_desc_span(link->tx, &tx_first, &tx_end);
    /* printf, not ESP_LOG: the host runs with logging disabled. */
    LOG("i2slink SRAM placement (bank): link %08x..%08x (%d..%d), RX ring %08x (%d), "
           "RX descriptors %08x..%08x (%d..%d), TX descriptors %08x..%08x (%d..%d)\n",
           (unsigned)start, (unsigned)end, sram_bank(start), sram_bank(end - 1U),
           (unsigned)ring, sram_bank(ring),
           (unsigned)rx_first, (unsigned)rx_end, sram_bank(rx_first), sram_bank(rx_end - 1U),
           (unsigned)tx_first, (unsigned)tx_end, sram_bank(tx_first), sram_bank(tx_end - 1U));
}

esp_err_t i2slink_init(i2slink_t **out, const i2slink_config_t *config)
{
    if (!out || !config || (config->i2s_port != I2S_NUM_0 && config->i2s_port != I2S_NUM_1)) {
        return ESP_ERR_INVALID_ARG;
    }
    runs_init();
    i2slink_t *link = (i2slink_t *)heap_caps_aligned_calloc(LINK_ALIGN, 1U, sizeof(*link),
                                               MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!link) {
        return ESP_ERR_NO_MEM;
    }
    const bool source = config->clock == I2SLINK_CLOCK_SOURCE;
    tx_cycle_init(link, source ? TAG_SOURCE : TAG_FOLLOWER);
    link->report_gen = (uint8_t)esp_random();
    link->seen_gen = 0x100U;
    link->sync_marker = -1;
    *(uint32_t *)(link->tx_cycle + OFF_REPORT) = report_encode(REPORT_NO_LOCK, link->report_gen);
    *(uint32_t *)(link->tx_cycle + OFF_REPORT2) = report2_encode(0U, 0U, 0U, 0U);
    if (config->tx_auto_sync && !I2SLINK_TX_AUTO_SYNC_CODE) {
        heap_caps_free(link);
        return ESP_ERR_NOT_SUPPORTED;   /* built without it (I2SLINK_TX_AUTO_SYNC_CODE) */
    }
    if (config->tx_auto_sync && !config->loopback) {
        link->tx_logical = (uint8_t *)heap_caps_malloc(N, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!TX_SYNC(link)) {
            heap_caps_free(link);
            return ESP_ERR_NO_MEM;
        }
        memcpy(link->tx_logical, link->tx_cycle, N);   /* shift 0: the same */
        link->slot_tune = true;   /* slot tuning: on */
        link->slot_fast_cycles = SLOT_FAST_US * CPU_MHZ;
        link->tx_seen_desc = -1;
    }
    /* RX expects the peer's tag, or in loopback this end's own. */
    memcpy(link->rx_tag, source != config->loopback ? TAG_FOLLOWER : TAG_SOURCE, 4U);
    memset(link->rx_ring, 0x5C, N);
    link->sync_timeout_cycles = (config->sync_timeout_us ? config->sync_timeout_us
                                                         : 4U * I2SLINK_CYCLE_US) * CPU_MHZ;
    /* A random start, so a restarted writer's versions don't repeat the
     * numbers a reader has already returned. */
    link->tx_sequence = esp_random();

    esp_err_t err = channels_create(link, config);
    if (err == ESP_OK) err = i2s_dma_rx_ring_attach(link->rx, link->rx_ring, N, link->rx_desc);
    if (err == ESP_OK) {
        err = i2s_dma_tx_ring_attach(link->tx, link->segments, I2SLINK_DESC_COUNT, link->tx_desc);
    }
    if (err == ESP_OK) {
        i2s_dma_rx_progress_t progress;
        err = i2s_dma_rx_progress_init(link->rx, &progress);
        for (unsigned index = 0U; err == ESP_OK && index < I2SLINK_DESC_COUNT; ++index) {
            link->dw0[index] = (volatile uint32_t *)progress.desc[index];
            link->dw0_armed[index] = *link->dw0[index] & ~LEN_MASK;
        }
    }
    if (err == ESP_OK) {
        if (source) {
            /* RX listening before TX starts BCLK (the bench's order). */
            err = i2s_dma_rx_ring_enable(link->rx);
            esp_rom_delay_us(100000U);
            if (err == ESP_OK) err = i2s_dma_tx_ring_enable(link->tx);
        } else {
            err = i2s_dma_tx_ring_enable(link->tx);
            if (err == ESP_OK) err = i2s_dma_rx_ring_enable(link->rx);
        }
    }
    if (err != ESP_OK) {
        i2slink_deinit(link);
        return err;
    }
    ESP_LOGI(TAG, "%s on I2S%d: %u frames/s (%d MHz source), BCLK GPIO%d, TX GPIO%d, RX GPIO%d%s%s; "
             "%u-byte cycle (%u us), %u x %u-byte descriptors, %u-byte payload",
             source ? "clock source" : "clock follower", config->i2s_port,
             (unsigned)I2SLINK_FRAMES_PER_SECOND,
             CLOCK_SOURCE_MHZ, config->bclk_gpio, config->tx_gpio, config->rx_gpio,
             I2SLINK_RX_BCLK_INVERT ? " (RX on falling edge)" : "",
             config->loopback ? ", loopback" : TX_SYNC(link) ? ", TX frame auto sync" : "", (unsigned)N,
             (unsigned)I2SLINK_CYCLE_US, (unsigned)I2SLINK_DESC_COUNT, (unsigned)DESC_BYTES,
             (unsigned)I2SLINK_PAYLOAD_BYTES);
    log_placement(link);
    *out = link;
    return ESP_OK;
}

void i2slink_deinit(i2slink_t *link)
{
    if (!link) {
        return;
    }
    /* The descriptors live in the struct: take them back from the driver,
     * which would free them. */
    if (link->tx) {
        i2s_channel_disable(link->tx);   /* fails harmlessly if never enabled */
        i2s_dma_ring_release_descriptors(link->tx, link->tx_desc);
        i2s_del_channel(link->tx);
    }
    if (link->rx) {
        i2s_channel_disable(link->rx);
        i2s_dma_ring_release_descriptors(link->rx, link->rx_desc);
        i2s_del_channel(link->rx);
    }
    heap_caps_free(link->tx_logical);
    heap_caps_free(link);
}
