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
#define MEMW() __asm__ volatile("memw" ::: "memory")

struct i2slink {
    /* DMA memory (the struct is allocated DMA-capable, 16-aligned). */
    uint8_t tx_cycle[N] __attribute__((aligned(16)));
    uint8_t rx_ring[N] __attribute__((aligned(16)));
    uint8_t copy[N] __attribute__((aligned(16)));      /* ring layout */
    uint8_t search_raw[N] __attribute__((aligned(16)));
    uint8_t search_aligned[N];
    uint8_t decoded[I2SLINK_PAYLOAD_BYTES];   /* checked before the caller sees it */

    i2s_chan_handle_t tx, rx;
    i2s_dma_segment_t segments[I2SLINK_DESC_COUNT];
    volatile uint32_t *dw0[I2SLINK_DESC_COUNT];
    uint32_t dw0_armed[I2SLINK_DESC_COUNT];   /* word 0 with length 0 */
    char rx_tag[4];
    uint32_t sync_timeout_cycles;

    /* Writer */
    uint32_t tx_sequence;

    /* Reader: lock */
    bool locked;
    uint8_t phase, lane;
    uint16_t marker;
    uint16_t region_start, region_length;
    volatile uint32_t *end_dw0, *guard_dw0;   /* tripwires E and G */
    uint32_t end_armed, guard_armed;
    /* hint(): the raw ring bits of T's low byte, as of the last good read */
    const volatile uint32_t *t_word[2];
    uint32_t t_mask[2], t_bits[2];

    uint32_t last_read;
    bool have_read;

    i2slink_stats_t stats;
};

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

IRAM_ATTR void i2slink_write(i2slink_t *link, const uint8_t payload[I2SLINK_PAYLOAD_BYTES])
{
    uint32_t sequence = link->tx_sequence + 1U;
    if (sequence == 0U) {
        sequence = 1U;
    }
    link->tx_sequence = sequence;
    const uint32_t crc = esp_rom_crc32_le(0U, payload, I2SLINK_PAYLOAD_BYTES);
    uint8_t *cycle = link->tx_cycle;

    /* T first (sampled last), payload and CRC, then H (sampled first). */
    *(volatile uint32_t *)(cycle + OFF_T) = sequence;
    MEMW();
    const uint8_t *in = payload;
    for (size_t run = 0U; run < RUN_COUNT; ++run) {
        memcpy(cycle + runs[run].offset, in, runs[run].length);
        in += runs[run].length;
    }
    *(volatile uint32_t *)(cycle + OFF_CRC) = crc;
    MEMW();
    *(volatile uint32_t *)(cycle + OFF_H) = sequence;
    MEMW();
    ++link->stats.writes;
}

/* ------------------------------------------------------------------------ */
/* Tripwires                                                                */
/* ------------------------------------------------------------------------ */
/* E is the RX descriptor holding the region's last raw byte: its length
 * appears when the beam leaves the region. G (the guard) is the last
 * descriptor of the gap but one: its length appears a whole spare
 * descriptor before the beam reaches the region. A length means something
 * only if it was cleared ("armed") after the beam's previous crossing, so:
 *   E clear, G clear: no crossing since arming
 *   E clear, G set:   the beam entered the region: re-arm G
 *   E set,   G clear: the beam is in VSYNC: the window is open
 *   E set,   G set:   both crossed (we were late): re-arm both
 * G is re-armed alone only while E is clear, and E alone only while G is
 * clear, so "E set, G clear" always means the window is open. Arming writes
 * word 0 with length 0 (one store, no read-modify-write); the DMA writes
 * that word again only a lap later, and RX DMA ignores the length. A store
 * that lands just as the DMA writes back loses a crossing that came before
 * the window (G) or costs one window (E): never a false window. */
typedef enum { TW_QUIET, TW_REGION, TW_WINDOW, TW_LATE } tripwire_t;

static inline IRAM_ATTR tripwire_t tripwire_step(i2slink_t *link)
{
    const uint32_t e = *link->end_dw0 & LEN_MASK;   /* E first, then G */
    const uint32_t g = *link->guard_dw0 & LEN_MASK;
    if (!e) {
        if (g) {
            *link->guard_dw0 = link->guard_armed;
            return TW_REGION;
        }
        return TW_QUIET;
    }
    if (g) {
        *link->end_dw0 = link->end_armed;
        *link->guard_dw0 = link->guard_armed;
        return TW_LATE;
    }
    return TW_WINDOW;
}

static inline IRAM_ATTR void arm_both(i2slink_t *link)
{
    *link->end_dw0 = link->end_armed;
    *link->guard_dw0 = link->guard_armed;
}

static inline IRAM_ATTR bool t_changed(const i2slink_t *link)
{
    return (((*link->t_word[0] ^ link->t_bits[0]) & link->t_mask[0]) |
            ((*link->t_word[1] ^ link->t_bits[1]) & link->t_mask[1])) != 0U;
}

SIDECAR_HOT_ATTR bool i2slink_hint(i2slink_t *link)
{
    if (!link->locked) {
        ++link->stats.hints_true;
        return true;
    }
    const tripwire_t state = tripwire_step(link);
    if (state == TW_QUIET || state == TW_REGION) {
        sidecar_nez_delay(SIDECAR_POLL_DELAY);   /* callers poll: pace them */
        return false;                       /* no pass has ended since arming */
    }
    if (t_changed(link)) {
        ++link->stats.hints_true;
        return true;                        /* tripwires left for read() */
    }
    if (state == TW_WINDOW) {
        *link->end_dw0 = link->end_armed;   /* same data again: re-arm E only */
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Reader                                                                   */
/* ------------------------------------------------------------------------ */
/* The raw ring bits of T's low byte, as two (word, mask) pairs (the second
 * mask is 0 when one word holds them all). */
static void t_bits_set(i2slink_t *link)
{
    const size_t j = aligned_of(link, OFF_T);
    size_t byte_index[2] = {raw_high(link->lane, j), raw_low(link->lane, j)};
    uint8_t byte_mask[2] = {(uint8_t)(0xFFU >> link->phase),
                            (uint8_t)(link->phase ? (0xFFU << (8U - link->phase)) & 0xFFU : 0U)};
    const volatile uint32_t *ring32 = (const volatile uint32_t *)link->rx_ring;
    link->t_word[0] = &ring32[byte_index[0] / 4U];
    link->t_mask[0] = (uint32_t)byte_mask[0] << (8U * (byte_index[0] % 4U));
    link->t_word[1] = &ring32[byte_index[1] / 4U];
    link->t_mask[1] = (uint32_t)byte_mask[1] << (8U * (byte_index[1] % 4U));
    if (link->t_word[1] == link->t_word[0]) {
        link->t_mask[0] |= link->t_mask[1];
        link->t_mask[1] = 0U;
    }
}

/* Remember T's bits from the copy just verified, quieting hint(). */
static IRAM_ATTR void t_bits_save(i2slink_t *link)
{
    const uint32_t *copy32 = (const uint32_t *)link->copy;
    const volatile uint32_t *ring32 = (const volatile uint32_t *)link->rx_ring;
    for (unsigned index = 0U; index < 2U; ++index) {
        link->t_bits[index] = copy32[link->t_word[index] - ring32];
    }
}

static void region_set(i2slink_t *link)
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
    link->end_dw0 = link->dw0[end_desc];
    link->end_armed = link->dw0_armed[end_desc];
    link->guard_dw0 = link->dw0[guard_desc];
    link->guard_armed = link->dw0_armed[guard_desc];
    t_bits_set(link);
    /* hint() is true until the first read, so a version already waiting is
     * not stranded. */
    link->t_bits[0] = ~*link->t_word[0];
    link->t_bits[1] = ~*link->t_word[1];
}

static IRAM_ATTR void copy_region(i2slink_t *link)
{
    const volatile uint16_t *from = (const volatile uint16_t *)link->rx_ring;
    uint16_t *to = (uint16_t *)link->copy;
    size_t word = link->region_start / 2U;
    for (size_t index = 0U; index < link->region_length / 2U; ++index) {
        to[word] = from[word];
        if (++word == N / 2U) {
            word = 0U;
        }
    }
}

static inline IRAM_ATTR bool framing_ok(const i2slink_t *link)
{
    return wire_framing_ok(link->copy, link->phase, link->lane, link->marker);
}

/* Search one snapshot of the ring at all 16 bit offsets, starting from the
 * last lock's. */
static IRAM_ATTR bool search(i2slink_t *link)
{
    MEMW();
    memcpy(link->search_raw, link->rx_ring, N);
    uint8_t phase, lane;
    uint16_t marker;
    if (!wire_search(link->search_raw, link->search_aligned, link->rx_tag, link->phase,
                     link->lane, &phase, &lane, &marker)) {
        return false;
    }
    link->phase = phase;
    link->lane = lane;
    link->marker = marker;
    region_set(link);
    arm_both(link);
    link->locked = true;
    ++link->stats.locks;
    return true;
}

static inline IRAM_ATTR i2slink_status_t finish(i2slink_t *link, i2slink_status_t status)
{
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

static IRAM_ATTR copy_t take_window(i2slink_t *link, uint32_t *h_out)
{
    const unsigned irq = portSET_INTERRUPT_MASK_FROM_ISR();
    copy_region(link);
    MEMW();
    const bool guard_crossed = (*link->guard_dw0 & LEN_MASK) != 0U;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    *link->end_dw0 = link->end_armed;       /* consume this window */
    if (guard_crossed) {
        ++link->stats.tripped;
        arm_both(link);
        return COPY_NONE;
    }
    if (!framing_ok(link)) {
        ++link->stats.framing;
        link->locked = false;
        return COPY_FRAMING;
    }
    ++link->stats.windows;
    const uint32_t h = decode_u32(link, link->copy, OFF_H);
    const uint32_t t = decode_u32(link, link->copy, OFF_T);
    if (h != t) {
        ++link->stats.torn;
        return COPY_TORN;
    }
    if (h == 0U || (link->have_read && h == link->last_read)) {
        ++link->stats.stale;
        t_bits_save(link);
        return COPY_STALE;
    }
    wire_decode_payload(link->copy, link->phase, link->lane, link->marker, link->decoded);
    const uint32_t crc = decode_u32(link, link->copy, OFF_CRC);
    if (esp_rom_crc32_le(0U, link->decoded, I2SLINK_PAYLOAD_BYTES) != crc) {
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
    t_bits_save(link);
    *h_out = h;
    return COPY_NEW;
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
        if (!link->locked) {
            first = false;
            if (!search(link)) {
                fault = I2SLINK_NO_LOCK;
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
                switch (take_window(link, &h)) {
                case COPY_NEW:
                    memcpy(payload, link->decoded, I2SLINK_PAYLOAD_BYTES);
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

void i2slink_stats(const i2slink_t *link, i2slink_stats_t *stats)
{
    *stats = link->stats;
    stats->bit_phase = link->phase;
    stats->byte_lane = link->lane;
    stats->marker_offset = link->marker;
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
    std.gpio_cfg.invert_flags.bclk_inv = config->rx_bclk_invert;
    RETURN_ON_ERROR(i2s_channel_init_std_mode(link->rx, &std));
    chan = chan_config(config->i2s_port, source ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
    RETURN_ON_ERROR(i2s_new_channel(&chan, &link->tx, NULL));
    std = std_config(config, config->tx_gpio, I2S_GPIO_UNUSED);
    RETURN_ON_ERROR(i2s_channel_init_std_mode(link->tx, &std));
    /* The pad is shared: keep its input enabled for RX (the source's TX
     * drives it as an output). */
    return gpio_input_enable(config->bclk_gpio);
}

esp_err_t i2slink_init(i2slink_t **out, const i2slink_config_t *config)
{
    if (!out || !config || (config->i2s_port != I2S_NUM_0 && config->i2s_port != I2S_NUM_1)) {
        return ESP_ERR_INVALID_ARG;
    }
    runs_init();
    i2slink_t *link = heap_caps_aligned_calloc(16U, 1U, sizeof(*link),
                                               MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!link) {
        return ESP_ERR_NO_MEM;
    }
    const bool source = config->clock == I2SLINK_CLOCK_SOURCE;
    tx_cycle_init(link, source ? TAG_SOURCE : TAG_FOLLOWER);
    memcpy(link->rx_tag, source ? TAG_FOLLOWER : TAG_SOURCE, 4U);
    memset(link->rx_ring, 0x5C, N);
    link->sync_timeout_cycles = (config->sync_timeout_us ? config->sync_timeout_us
                                                         : 4U * I2SLINK_CYCLE_US) * CPU_MHZ;
    /* A random start, so a restarted writer's versions don't repeat the
     * numbers a reader has already returned. */
    link->tx_sequence = esp_random();

    esp_err_t err = channels_create(link, config);
    if (err == ESP_OK) err = i2s_dma_rx_ring_attach(link->rx, link->rx_ring, N);
    if (err == ESP_OK) err = i2s_dma_tx_ring_attach(link->tx, link->segments, I2SLINK_DESC_COUNT);
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
    ESP_LOGI(TAG, "%s on I2S%d: %u frames/s (%d MHz source), BCLK GPIO%d, TX GPIO%d, RX GPIO%d%s; "
             "%u-byte cycle (%u us), %u x %u-byte descriptors, %u-byte payload",
             source ? "clock source" : "clock follower", config->i2s_port,
             (unsigned)I2SLINK_FRAMES_PER_SECOND,
             CLOCK_SOURCE_MHZ, config->bclk_gpio, config->tx_gpio, config->rx_gpio,
             config->rx_bclk_invert ? " (RX on falling edge)" : "", (unsigned)N,
             (unsigned)I2SLINK_CYCLE_US, (unsigned)I2SLINK_DESC_COUNT, (unsigned)DESC_BYTES,
             (unsigned)I2SLINK_PAYLOAD_BYTES);
    *out = link;
    return ESP_OK;
}

void i2slink_deinit(i2slink_t *link)
{
    if (!link) {
        return;
    }
    if (link->tx) {
        i2s_channel_disable(link->tx);   /* fails harmlessly if never enabled */
        i2s_del_channel(link->tx);
    }
    if (link->rx) {
        i2s_channel_disable(link->rx);
        i2s_del_channel(link->rx);
    }
    heap_caps_free(link);
}
