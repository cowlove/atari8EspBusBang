// CSIM: the ESP32-S3 I2S peripheral and its GDMA, as far as the real link
// code (components/sidecar_link/i2slink.c, built unchanged) uses them: the
// I2S standard-mode driver calls and the i2s_dma_ring.h API, on emulated
// channels that run in simulated time.
//
// - Clock: a BCLK domain is the set of channels whose BCLK pins are one
//   net (the same GPIO, or joined by csim_i2slink_wire()). It runs while
//   its master channel (the clock source's TX) is enabled, at
//   32 x I2SLINK_FRAMES_PER_SECOND bits per second, and counts its bits.
// - Channels start where they're enabled: a channel takes the domain's
//   bit count at its enable (or at the clock's start, if it was enabled
//   before), plus a seeded random delay of up to two wire cycles, as its
//   sample boundary. So each RX hears its peer's TX at an arbitrary bit
//   offset, as on the wire, and the reader's lock search finds the bit
//   phase, byte lane and marker.
// - Data: a TX sends 16-bit samples MSB first, fetched little-endian from
//   memory by walking its descriptor chain (each descriptor's length bytes
//   from its buf, then next), as the GDMA does, so a TX built from
//   scattered buffers works too. An RX assembles each 16 bits from the
//   wire (the TX on its data net, idle-low without one) and stores them
//   little-endian, walking its chain: each descriptor's size bytes into
//   its buf; when one fills, its length field is written back (and owner
//   cleared), which is what the link's tripwires watch.
// - Time: the engines catch up on every clock advance (csimClockHook,
//   csim.h), between the slices the coroutines run: code sees the
//   hardware as of its slice's start, and its stores (a TX cycle being
//   rewritten, a descriptor being armed) take effect for what the DMA does
//   after. A write landing while the TX sends the stamped region tears
//   that pass on the wire, as it would on the chip.
//
// Not modeled: FIFO depths and DMA prefetch, owner checking, EOF
// interrupts (the rings mask them), sampling-edge choice, wire delay.

#include "csim.h"
#include "driver/i2s_std.h"
#include "i2s_dma_ring.h"
#include "i2slink.h"
#include "i2slink_csim.h"

#include <stdio.h>
#include <algorithm>
#include <vector>

struct csim_i2s_chan {
    int port = 0;
    bool tx = false;
    i2s_role_t role = I2S_ROLE_SLAVE;
    uint32_t descNum = 0;
    int bclk = I2S_GPIO_UNUSED, data = I2S_GPIO_UNUSED;
    bool enabled = false;
    bool started = false;      // has its sample boundary (gStart)
    uint64_t gStart = 0;       // domain bit count at its first sample
    // Descriptor chain and the DMA's place in it.
    lldesc_t *first = nullptr;
    lldesc_t *cur = nullptr;
    uint32_t offset = 0;
    std::vector<lldesc_t *> descs;   // as attached, for the span and progress
    // TX: samples fetched so far (k), the last two cached.
    uint64_t fetched = 0;
    uint64_t cacheK[2] = {UINT64_MAX, UINT64_MAX};
    uint16_t cacheV[2] = {0, 0};
    // RX: samples stored so far.
    uint64_t stored = 0;
};

namespace {

struct Wire {
    int a, b;
};
std::vector<Wire> wires;
std::vector<csim_i2s_chan *> channels;

// The BCLK bit count of a domain, named by its master's BCLK pin.
struct Domain {
    int bclk;
    csim_i2s_chan *master;
    uint64_t baseBits;   // bits counted before the current run
    uint64_t runStart;   // CCOUNT when the current run began
    bool running;
};
std::vector<Domain> domains;

constexpr uint64_t kBitsPerSecond = 32ULL * I2SLINK_FRAMES_PER_SECOND;
constexpr uint64_t kTicksPerSecond = 240000000ULL;

bool sameNet(int a, int b)
{
    if (a == I2S_GPIO_UNUSED || b == I2S_GPIO_UNUSED) {
        return false;
    }
    if (a == b) {
        return true;
    }
    for (const Wire &wire : wires) {
        if ((wire.a == a && wire.b == b) || (wire.a == b && wire.b == a)) {
            return true;
        }
    }
    return false;
}

uint64_t domainBits(const Domain &d, uint64_t now)
{
    return d.baseBits + (d.running ? (now - d.runStart) * kBitsPerSecond / kTicksPerSecond : 0U);
}

Domain *domainOf(const csim_i2s_chan *ch)
{
    for (Domain &d : domains) {
        if (sameNet(d.bclk, ch->bclk)) {
            return &d;
        }
    }
    return nullptr;
}

// Where a channel's first sample falls is up to the hardware (slave
// framing, when software got to the enable): each start is a random number
// of bits later, up to two wire cycles (CSIM_SEED varies it), so each RX
// hears its peer at a random bit phase, byte lane and ring position, and
// each restart at a new one. (Without it every channel started on the
// clock's first bit: one perfectly aligned geometry, every run.)
uint32_t startJitterBits()
{
    static uint32_t state = csim_seeded(0x1B873593U);
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state % (2U * 8U * I2SLINK_CYCLE_BYTES);
}

// A channel enabled while its clock runs starts then (plus the jitter);
// one enabled before waits for the clock.
void startIfClocked(csim_i2s_chan *ch, uint64_t now)
{
    Domain *d = domainOf(ch);
    if (ch->enabled && !ch->started && d && d->running) {
        ch->started = true;
        ch->gStart = domainBits(*d, now) + startJitterBits();
    }
}

// TX: the next sample from memory, walking the chain.
uint16_t fetchNext(csim_i2s_chan *tx)
{
    if (!tx->cur) {
        return 0;
    }
    if (tx->offset + 2U > tx->cur->length) {
        // An empty or exhausted descriptor: the next (a TX ring loops).
        lldesc_t *next = tx->cur->next;
        tx->cur = next ? next : tx->first;
        tx->offset = 0;
        if (!tx->cur || tx->cur->length < 2U) {
            return 0;
        }
    }
    const volatile uint8_t *buf = tx->cur->buf;
    const uint16_t value = (uint16_t)(buf[tx->offset] | (buf[tx->offset + 1U] << 8));
    tx->offset += 2U;
    return value;
}

// TX sample k (k never goes back by more than one).
uint16_t txSample(csim_i2s_chan *tx, uint64_t k)
{
    for (int slot = 0; slot < 2; ++slot) {
        if (tx->cacheK[slot] == k) {
            return tx->cacheV[slot];
        }
    }
    uint16_t value = 0;
    while (tx->fetched <= k) {
        value = fetchNext(tx);
        const int slot = (int)(tx->fetched & 1U);
        tx->cacheK[slot] = tx->fetched;
        tx->cacheV[slot] = value;
        ++tx->fetched;
    }
    return tx->cacheK[k & 1U] == k ? tx->cacheV[k & 1U] : 0;
}

// The 16 wire bits from domain bit g on, as the TX on rx's data net sends
// them (zeros where nothing drives the line).
uint16_t wireWord(csim_i2s_chan *rx, uint64_t g)
{
    csim_i2s_chan *tx = nullptr;
    for (csim_i2s_chan *ch : channels) {
        if (ch->tx && ch->enabled && ch->started && sameNet(ch->data, rx->data) &&
            domainOf(ch) == domainOf(rx)) {
            tx = ch;
            break;
        }
    }
    if (!tx || g + 16U <= tx->gStart) {
        return 0;
    }
    if (g < tx->gStart) {
        // The TX starts mid-word: idle bits, then its first sample's top bits.
        const unsigned idle = (unsigned)(tx->gStart - g);
        return (uint16_t)(txSample(tx, 0) >> idle);
    }
    const uint64_t rel = g - tx->gStart;
    const uint64_t k = rel / 16U;
    const unsigned shift = (unsigned)(rel % 16U);
    const uint16_t high = txSample(tx, k);
    if (shift == 0U) {
        return high;
    }
    const uint16_t low = txSample(tx, k + 1U);
    return (uint16_t)((high << shift) | (low >> (16U - shift)));
}

// RX: store a sample; a descriptor that fills gets its length written back.
void storeNext(csim_i2s_chan *rx, uint16_t value)
{
    if (!rx->cur) {
        return;
    }
    volatile uint8_t *buf = const_cast<volatile uint8_t *>(rx->cur->buf);
    buf[rx->offset] = (uint8_t)value;
    buf[rx->offset + 1U] = (uint8_t)(value >> 8);
    rx->offset += 2U;
    if (rx->offset >= rx->cur->size) {
        rx->cur->length = rx->offset;   // the GDMA's write-back
        rx->cur->owner = 0U;
        lldesc_t *next = rx->cur->next;
        rx->cur = next ? next : nullptr;   // a chain that ends stops the RX
        rx->offset = 0;
    }
}

void advance()
{
    const uint64_t now = CSIM_PEEK_CCOUNT64();
    for (csim_i2s_chan *rx : channels) {
        if (rx->tx || !rx->enabled) {
            continue;
        }
        startIfClocked(rx, now);
        Domain *d = domainOf(rx);
        if (!rx->started || !d) {
            continue;
        }
        const uint64_t g = domainBits(*d, now);
        while (rx->gStart + 16U * (rx->stored + 1U) <= g) {
            storeNext(rx, wireWord(rx, rx->gStart + 16U * rx->stored));
            ++rx->stored;
        }
    }
}

struct Install {
    Install() { csimClockHook = advance; }
} install;

}  // namespace

void csim_i2slink_wire(int gpio_a, int gpio_b)
{
    wires.push_back({gpio_a, gpio_b});
}

extern "C" void sidecar_sim_delay_cycles(uint32_t cycles)
{
    csim_wait_until(CSIM_PEEK_CCOUNT64() + cycles);
}

// Time passing inside a critical section (SIDECAR_SIM_ELAPSE): the clock
// moves, so the hardware does (the DMA keeps writing a ring being copied),
// but no other code runs.
extern "C" void sidecar_sim_elapse_cycles(uint32_t cycles)
{
    CSIM_ADVANCE_CCOUNT(cycles);
}

// ---- The I2S driver ----

esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx,
                          i2s_chan_handle_t *rx)
{
    if (!config || (!tx && !rx)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int dir = 0; dir < 2; ++dir) {
        i2s_chan_handle_t *out = dir ? rx : tx;
        if (!out) {
            continue;
        }
        csim_i2s_chan *ch = new csim_i2s_chan;
        ch->port = config->id;
        ch->tx = dir == 0;
        ch->role = config->role;
        ch->descNum = config->dma_desc_num;
        channels.push_back(ch);
        *out = ch;
    }
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t ch, const i2s_std_config_t *config)
{
    if (!ch || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    ch->bclk = config->gpio_cfg.bclk;
    ch->data = ch->tx ? config->gpio_cfg.dout : config->gpio_cfg.din;
    return ESP_OK;
}

static void enable(csim_i2s_chan *ch)
{
    advance();   // the hardware up to now, before anything changes
    const uint64_t now = CSIM_PEEK_CCOUNT64();
    ch->enabled = true;
    ch->started = false;
    ch->cur = ch->first;
    ch->offset = 0;
    ch->fetched = 0;
    ch->stored = 0;
    ch->cacheK[0] = ch->cacheK[1] = UINT64_MAX;
    if (ch->tx && ch->role == I2S_ROLE_MASTER) {
        Domain *d = nullptr;
        for (Domain &existing : domains) {
            if (existing.bclk == ch->bclk) {
                d = &existing;
            }
        }
        if (!d) {
            domains.push_back({ch->bclk, ch, 0, now, false});
            d = &domains.back();
        }
        d->master = ch;
        d->runStart = now;
        d->running = true;
        for (csim_i2s_chan *other : channels) {
            startIfClocked(other, now);   // everything waiting on this clock
        }
    }
    startIfClocked(ch, now);
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t ch)
{
    if (!ch || !ch->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    advance();
    const uint64_t now = CSIM_PEEK_CCOUNT64();
    ch->enabled = false;
    for (Domain &d : domains) {
        if (d.master == ch && d.running) {
            d.baseBits = domainBits(d, now);   // the clock stops
            d.running = false;
        }
    }
    return ESP_OK;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t ch)
{
    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ch->enabled) {
        i2s_channel_disable(ch);
    }
    for (Domain &d : domains) {
        if (d.master == ch) {
            d.master = nullptr;
        }
    }
    channels.erase(std::remove(channels.begin(), channels.end(), ch), channels.end());
    delete ch;
    return ESP_OK;
}

// ---- i2s_dma_ring.h ----

static void pointDescriptor(lldesc_t *descriptor, uint8_t *buffer, size_t size)
{
    descriptor->buf = buffer;
    descriptor->length = size;
    descriptor->size = size;
    descriptor->owner = 1U;
    descriptor->eof = 1U;
    descriptor->sosf = 0U;
    descriptor->offset = 0U;
}

// Point the channel at caller descriptors, linked in a loop (the driver's
// layout), as i2s_dma_ring.c's adopt_descriptors() does.
static void adopt(csim_i2s_chan *ch, lldesc_t *descriptors, size_t count)
{
    ch->descs.clear();
    for (size_t index = 0; index < count; ++index) {
        memset(&descriptors[index], 0, sizeof(descriptors[index]));
        descriptors[index].next = &descriptors[(index + 1U) % count];
        ch->descs.push_back(&descriptors[index]);
    }
    ch->first = ch->cur = count ? &descriptors[0] : nullptr;
    ch->offset = 0;
}

esp_err_t i2s_dma_tx_ring_attach(i2s_chan_handle_t ch, const i2s_dma_segment_t *segments,
                                 size_t count, lldesc_t *descriptors)
{
    if (!ch || !segments || !ch->tx || !descriptors || ch->descNum != count ||
        ((uintptr_t)descriptors & 0x03U)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!segments[index].buffer || !segments[index].size || segments[index].size % 4U ||
            ((uintptr_t)segments[index].buffer & 0x03U) || segments[index].size > 4095U) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    adopt(ch, descriptors, count);
    for (size_t index = 0; index < count; ++index) {
        pointDescriptor(&descriptors[index], segments[index].buffer, segments[index].size);
    }
    return ESP_OK;
}

esp_err_t i2s_dma_rx_ring_attach(i2s_chan_handle_t ch, uint8_t *buffer, size_t size,
                                 lldesc_t *descriptors)
{
    if (!ch || !buffer || ch->tx || !descriptors || size == 0U || (size & 3U) ||
        ((uintptr_t)buffer & 0x0FU) || ch->descNum < 2U || ((uintptr_t)descriptors & 0x03U)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t each = size / ch->descNum;
    if (each * ch->descNum != size || (each & 3U) || each > 4095U) {
        return ESP_ERR_INVALID_SIZE;
    }
    adopt(ch, descriptors, ch->descNum);
    for (size_t index = 0; index < ch->descNum; ++index) {
        pointDescriptor(&descriptors[index], buffer + index * each, each);
    }
    return ESP_OK;
}

esp_err_t i2s_dma_tx_ring_enable(i2s_chan_handle_t ch)
{
    if (!ch || !ch->tx || !ch->first) {
        return ESP_ERR_INVALID_STATE;
    }
    enable(ch);
    return ESP_OK;
}

esp_err_t i2s_dma_rx_ring_enable(i2s_chan_handle_t ch)
{
    if (!ch || ch->tx || !ch->first) {
        return ESP_ERR_INVALID_STATE;
    }
    enable(ch);
    return ESP_OK;
}

esp_err_t i2s_dma_rx_ring_restart(i2s_chan_handle_t ch)
{
    if (!ch || ch->tx) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ch->enabled) {
        i2s_channel_disable(ch);
    }
    enable(ch);   // back at descriptor 0
    return ESP_OK;
}

void i2s_dma_ring_release_descriptors(i2s_chan_handle_t ch, const lldesc_t *descriptors)
{
    (void)descriptors;
    if (ch) {
        ch->descs.clear();
        ch->first = ch->cur = nullptr;
    }
}

esp_err_t i2s_dma_desc_span(i2s_chan_handle_t ch, uintptr_t *first, uintptr_t *end)
{
    if (!ch || !first || !end || ch->descs.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    uintptr_t low = UINTPTR_MAX, high = 0U;
    for (lldesc_t *descriptor : ch->descs) {
        const uintptr_t address = (uintptr_t)descriptor;
        low = std::min(low, address);
        high = std::max(high, address + sizeof(lldesc_t));
    }
    *first = low;
    *end = high;
    return ESP_OK;
}

esp_err_t i2s_dma_rx_progress_init(i2s_chan_handle_t ch, i2s_dma_rx_progress_t *progress)
{
    if (!ch || !progress || ch->tx || ch->descs.size() > I2S_DMA_RX_PROGRESS_MAX_DESC) {
        return ESP_ERR_INVALID_ARG;
    }
    progress->count = ch->descs.size();
    for (size_t index = 0; index < progress->count; ++index) {
        ch->descs[index]->length = 0U;
        progress->desc[index] = ch->descs[index];
    }
    return ESP_OK;
}

uint32_t i2s_dma_rx_progress_poll(i2s_dma_rx_progress_t *progress)
{
    uint32_t completed = 0U;
    for (size_t index = 0; index < progress->count; ++index) {
        lldesc_t *descriptor = static_cast<lldesc_t *>(progress->desc[index]);
        if (descriptor->length != 0U) {
            descriptor->length = 0U;
            ++completed;
        }
    }
    return completed;
}

void i2s_dma_rx_debug_dump(i2s_chan_handle_t ch)
{
    (void)ch;
}

void i2s_dma_rx_snapshot(i2s_chan_handle_t ch, i2s_dma_rx_snapshot_t *snapshot)
{
    (void)ch;
    *snapshot = {};
    snapshot->ccount = (uint32_t)CSIM_PEEK_CCOUNT64();
}
