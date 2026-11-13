// CSIM model of i2slink.h: see i2slink_csim.h.
//
// Each end owns a TX cycle (the real 468-byte wire cycle, built with the
// real template and payload runs) and an RX ring. A write stores the new
// cycle into the connected peer's RX ring, shifted by a random number of
// bits chosen when the two ends connect, so the reader's lock search and
// bit-shift decode (i2slink_wire.h, shared with the hardware link) do real
// work. Single-threaded and cooperative: a write is never torn, and nothing
// arrives while a caller waits, so read() never loops; a wait only advances
// simulated time, so callers' timeout loops end.

#include "i2slink.h"
#include "i2slink_csim.h"
#include "i2slink_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <zlib.h>

namespace {

struct Wire {
    int a, b;
};
std::vector<Wire> wires;

// Each side's poll, by its TX GPIO: run when the other side's read finds
// nothing new (csim_i2slink_set_poll()).
struct Poll {
    int txGpio;
    void (*poll)(void);
    bool (*busy)(void);
    bool running;
};
std::vector<Poll> polls;

uint32_t rngState = 0x2545F491U;   // fixed seed: runs are reproducible
uint32_t rng()
{
    uint32_t x = rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rngState = x;
}

uint32_t crc32Of(const uint8_t *data, size_t length)
{
    return (uint32_t)crc32(0UL, data, (uInt)length);
}

bool wired(int a, int b)
{
    for (const Wire &wire : wires) {
        if ((wire.a == a && wire.b == b) || (wire.a == b && wire.b == a)) {
            return true;
        }
    }
    return false;
}

}  // namespace

struct i2slink {
    i2slink_config_t config;
    char rxTag[4];
    uint8_t txCycle[N];
    uint8_t rxRing[N];
    uint8_t aligned[N];   // search scratch
    uint8_t decoded[I2SLINK_PAYLOAD_BYTES];

    i2slink_t *peer = nullptr;
    unsigned rxShiftBits = 0;    // the bit shift of the peer's cycle in rxRing
    uint32_t rxVersion = 0;      // cycles stored into rxRing
    uint32_t seenVersion = 0;    // rxVersion at the last clean read

    uint32_t txSequence = 0;

    bool locked = false;
    uint8_t phase = 0, lane = 0;
    uint16_t marker = 0;
    uint32_t lastRead = 0;
    bool haveRead = false;

    i2slink_stats_t stats = {};
};

namespace {

std::vector<i2slink_t *> links;

// Store from's TX cycle into to's RX ring, as the RX DMA would see it: each
// 16-bit sample goes out MSB first (wire byte k is memory byte k ^ 1 on both
// ends), and the received bit stream is the sent one delayed by
// rxShiftBits.
void transmit(const i2slink_t *from, i2slink_t *to)
{
    uint8_t wireBytes[N];
    for (size_t k = 0; k < N; ++k) {
        wireBytes[k] = from->txCycle[k ^ 1U];
    }
    const size_t totalBits = 8U * N;
    for (size_t q = 0; q < N; ++q) {
        const size_t bit = (8U * q + totalBits - to->rxShiftBits) % totalBits;
        const unsigned shift = bit % 8U;
        const uint8_t high = wireBytes[bit / 8U];
        const uint8_t low = wireBytes[(bit / 8U + 1U) % N];
        const uint8_t received = shift ? (uint8_t)((high << shift) | (low >> (8U - shift))) : high;
        to->rxRing[q ^ 1U] = received;
    }
    ++to->rxVersion;
}

void connect(i2slink_t *a, i2slink_t *b)
{
    a->peer = b;
    b->peer = a;
    // A new connection, a new bit alignment each way: readers must relock.
    a->rxShiftBits = rng() % (8U * N);
    b->rxShiftBits = rng() % (8U * N);
    transmit(a, b);
    transmit(b, a);
}

i2slink_status_t finish(i2slink_t *link, i2slink_status_t status)
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

bool search(i2slink_t *link)
{
    uint8_t phase, lane;
    uint16_t marker;
    if (!wire_search(link->rxRing, link->aligned, link->rxTag, link->phase, link->lane, &phase,
                     &lane, &marker)) {
        return false;
    }
    link->phase = phase;
    link->lane = lane;
    link->marker = marker;
    link->locked = true;
    ++link->stats.locks;
    return true;
}

// Waiting can't bring anything new in a cooperative simulation, but it
// should still cost simulated time, so callers' deadline loops end.
void waitFor(uint32_t waitUs)
{
    if (waitUs == I2SLINK_WAIT_FOREVER) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            printf("i2slink csim: WAIT_FOREVER read with nothing new would hang; "
                   "returning TIMEOUT\n");
        }
        return;
    }
    CSIM_ADVANCE_CCOUNT(waitUs * 240U);
}

}  // namespace

void csim_i2slink_wire(int gpio_a, int gpio_b)
{
    wires.push_back({gpio_a, gpio_b});
}

void csim_i2slink_set_poll(int tx_gpio, void (*poll)(void), bool (*busy)(void))
{
    polls.push_back({tx_gpio, poll, busy, false});
}

extern "C" esp_err_t i2slink_init(i2slink_t **out, const i2slink_config_t *config)
{
    if (!out || !config || (config->i2s_port != 0 && config->i2s_port != 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    runs_init();
    i2slink_t *link = new i2slink_t;
    link->config = *config;
    const bool source = config->clock == I2SLINK_CLOCK_SOURCE;
    wire_cycle_init(link->txCycle, source ? TAG_SOURCE : TAG_FOLLOWER);
    memcpy(link->rxTag, source != config->loopback ? TAG_FOLLOWER : TAG_SOURCE, 4U);
    memset(link->rxRing, 0x5C, N);
    link->txSequence = rng();
    if (config->loopback) {
        connect(link, link);   // RX hears this end's own TX
    }
    for (i2slink_t *other : links) {
        if (link->peer) {
            break;
        }
        if (!other->peer && wired(config->tx_gpio, other->config.rx_gpio) &&
            wired(other->config.tx_gpio, config->rx_gpio)) {
            connect(link, other);
            break;
        }
    }
    links.push_back(link);
    printf("i2slink csim: %s TX GPIO%d RX GPIO%d: %s\n", source ? "clock source" : "clock follower",
           config->tx_gpio, config->rx_gpio,
           config->loopback ? "loopback" : link->peer ? "connected" : "waiting for a peer");
    *out = link;
    return ESP_OK;
}

extern "C" void i2slink_deinit(i2slink_t *link)
{
    if (!link) {
        return;
    }
    if (link->peer) {
        link->peer->peer = nullptr;   // its RX ring keeps the last cycle, as a stopped DMA would
    }
    for (size_t index = 0; index < links.size(); ++index) {
        if (links[index] == link) {
            links.erase(links.begin() + index);
            break;
        }
    }
    delete link;
}

extern "C" void i2slink_write(i2slink_t *link, const uint8_t payload[I2SLINK_PAYLOAD_BYTES])
{
    uint32_t sequence = link->txSequence + 1U;
    if (sequence == 0U) {
        sequence = 1U;
    }
    link->txSequence = sequence;
    uint8_t *cycle = link->txCycle;
    memcpy(cycle + OFF_T, &sequence, 4U);
    const uint8_t *in = payload;
    for (size_t run = 0; run < RUN_COUNT; ++run) {
        memcpy(cycle + runs[run].offset, in, runs[run].length);
        in += runs[run].length;
    }
    const uint32_t crc = crc32Of(payload, I2SLINK_PAYLOAD_BYTES);
    memcpy(cycle + OFF_CRC, &crc, 4U);
    memcpy(cycle + OFF_H, &sequence, 4U);
    ++link->stats.writes;
    if (link->peer) {
        transmit(link, link->peer);
    }
}

// The peer's side gets a turn: its poll, unless it's mid-call (its own
// transact() is what's waiting further up the stack) or already running.
// True if it ran.
static bool runPeerPoll(i2slink_t *link)
{
    if (!link->peer || link->peer == link) {
        return false;
    }
    for (Poll &p : polls) {
        if (p.txGpio == link->peer->config.tx_gpio) {
            if (p.running || (p.busy && p.busy())) {
                return false;
            }
            p.running = true;
            p.poll();
            p.running = false;
            return true;
        }
    }
    return false;
}

static i2slink_status_t readOnce(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                                 uint32_t *sequence, uint32_t wait_us, bool lastTry);

extern "C" i2slink_status_t i2slink_read(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                                         uint32_t *sequence, uint32_t wait_us)
{
    // Nothing new, and the caller would wait: let the peer run (that's what
    // waiting would bring on hardware), then look again.
    i2slink_status_t status = readOnce(link, payload, sequence, wait_us, wait_us == 0);
    if (status == I2SLINK_TIMEOUT && wait_us != 0 && runPeerPoll(link)) {
        status = readOnce(link, payload, sequence, wait_us, true);
    } else if (status == I2SLINK_TIMEOUT && wait_us != 0) {
        waitFor(wait_us);
    }
    return status;
}

extern "C" bool i2slink_hint(i2slink_t *link)
{
    const bool hint = !link->locked || link->rxVersion != link->seenVersion;
    if (hint) {
        ++link->stats.hints_true;
    }
    return hint;
}

static i2slink_status_t readOnce(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                                 uint32_t *sequence, uint32_t wait_us, bool lastTry)
{
    if (!link->peer && link->rxVersion == 0U) {
        waitFor(wait_us);
        return finish(link, I2SLINK_STALLED);   // never received anything: no clock
    }
    if (link->locked && !wire_framing_ok(link->rxRing, link->phase, link->lane, link->marker)) {
        ++link->stats.framing;
        link->locked = false;
    }
    if (!link->locked && !search(link)) {
        waitFor(wait_us);
        return finish(link, I2SLINK_NO_LOCK);
    }
    ++link->stats.windows;
    ++link->stats.immediate;
    const uint32_t h = wire_decode_u32(link->rxRing, link->phase, link->lane, link->marker, OFF_H);
    const uint32_t t = wire_decode_u32(link->rxRing, link->phase, link->lane, link->marker, OFF_T);
    if (h != t) {
        ++link->stats.torn;   // can't happen here: writes are atomic
        return finish(link, I2SLINK_TORN);
    }
    if (h == 0U || (link->haveRead && h == link->lastRead)) {
        ++link->stats.stale;
        link->seenVersion = link->rxVersion;
        if (lastTry) {
            waitFor(wait_us);
        }
        return finish(link, I2SLINK_TIMEOUT);
    }
    wire_decode_payload(link->rxRing, link->phase, link->lane, link->marker, link->decoded);
    const uint32_t crc = wire_decode_u32(link->rxRing, link->phase, link->lane, link->marker, OFF_CRC);
    if (crc32Of(link->decoded, I2SLINK_PAYLOAD_BYTES) != crc) {
        ++link->stats.crc_errors;
        return finish(link, I2SLINK_CRC);
    }
    if (link->haveRead) {
        const uint32_t step = h - link->lastRead;
        if (step > (1U << 24)) {
            ++link->stats.jumps;
        } else if (step > 1U) {
            link->stats.skipped += step - 1U;
        }
    }
    link->lastRead = h;
    link->haveRead = true;
    link->seenVersion = link->rxVersion;
    memcpy(payload, link->decoded, I2SLINK_PAYLOAD_BYTES);
    if (sequence) {
        *sequence = h;
    }
    return finish(link, I2SLINK_NEW);
}

extern "C" void i2slink_debug_dump(const i2slink_t *link, const char *name)
{
    printf("i2slink csim %s: peer=%s shift=%u bits locked=%d phase=%u lane=%u marker=%u\n", name,
           link->peer ? "yes" : "no", link->rxShiftBits, link->locked, link->phase, link->lane,
           link->marker);
    for (size_t offset = 0; offset < N; offset += 32U) {
        printf("  %03u:", (unsigned)offset);
        for (size_t byte = offset; byte < offset + 32U && byte < N; ++byte) {
            printf(" %02x", link->rxRing[byte]);
        }
        printf("\n");
    }
}

extern "C" void i2slink_stats(const i2slink_t *link, i2slink_stats_t *stats)
{
    *stats = link->stats;
    stats->bit_phase = link->phase;
    stats->byte_lane = link->lane;
    stats->marker_offset = link->marker;
}

extern "C" void i2slink_memory(const i2slink_t *link, uintptr_t *start, size_t *size)
{
    *start = reinterpret_cast<uintptr_t>(link);
    *size = link ? sizeof(*link) : 0U;
}
