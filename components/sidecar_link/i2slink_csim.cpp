// CSIM model of i2slink.h: see i2slink_csim.h.
//
// Each end owns a TX cycle (the real 468-byte wire cycle, built with the
// real template and payload runs) and an RX ring. A write stores the new
// cycle into the connected peer's RX ring, shifted by a random number of
// bits chosen when the two ends connect, so the reader's lock search and
// bit-shift decode (i2slink_wire.h, shared with the hardware link) do real
// work.
//
// Timing follows the wire (I2SLINK_CYCLE_US per cycle; each direction gets
// a random cycle phase at connect): a write is carried by the first pass
// whose stamped region (H..T) starts after it, and the reader sees it at
// that pass's end, its next VSYNC; a write landing while a pass is sending
// the region tears that pass, so the version it replaced is lost (counted
// as torn on the reader). read() copies only in the beam window (VSYNC),
// so even a zero-wait read can wait up to a cycle; a link fault (no lock,
// no clock) is returned after sync_timeout_us, as on hardware. The waits
// are real waits in simulated time (csim_coroutine.h): on a coroutine they
// yield, so the peer and the 6502 run meanwhile.

#include "i2slink.h"
#include "i2slink_csim.h"
#include "i2slink_wire.h"

#include "csim_coroutine.h"

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

// Wire timing, in CCOUNT ticks (240 MHz).
constexpr uint64_t kTicksPerUs = 240U;
constexpr uint64_t kCycle = uint64_t(I2SLINK_CYCLE_BYTES) * 240000000ULL /
                            (4ULL * I2SLINK_FRAMES_PER_SECOND);
constexpr uint64_t kRegionStart = kCycle * OFF_H / N;   // into a pass: H goes out
constexpr uint64_t kVsync = kCycle * I2SLINK_VSYNC_BYTES / N;   // the beam window
// A lock search's CPU time on the S3, charged to the caller (who is busy
// meanwhile; on a coroutine the other side runs). Measured on the a24 bench
// pair, 2026-11-14: lockstress peek recoveries (one anchored search plus a
// copy) average 253 us; the exhaustive search took about 2.4 ms before the
// anchored one existed.
constexpr uint64_t kSearchAnchored = 250U * kTicksPerUs;
constexpr uint64_t kSearchFull = 2400U * kTicksPerUs;

inline uint64_t now()
{
    return CSIM_PEEK_CCOUNT64();
}

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

    // RX timing: pass k of the peer's TX cycle starts at rxPhase + k * kCycle
    // and ends (the reader's VSYNC window opens) one cycle later.
    uint64_t rxPhase = 0;
    // The peer's newest write, on its way: visible in rxRing from pendingAt.
    uint8_t pendingRing[N];
    bool pending = false;
    uint64_t pendingAt = 0;
    uint64_t sync_timeout = 0;   // CCOUNT ticks

    // hint()'s model of the hardware's T check: the ring's T byte under the
    // current lock, against a snapshot taken at the last clean copy (and
    // inverted at each new lock, so the first hint() after one is true);
    // and only once a pass has ended since that copy (the tripwires).
    uint8_t tSnap = 0;
    uint64_t armedAt = 0;
    // peek()'s pacing: a search, or a live framing check, at most once per
    // half cycle.
    bool lockedAtCopy = false;   // read(): this pass's copy was made under a lock
    bool peekSearched = false;
    uint64_t lastPeekSearch = 0, lastPeekFraming = 0;

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

// At exit: each live end's counters, to see what the wire model did.
void reportLinks()
{
    for (const i2slink_t *link : links) {
        const i2slink_stats_t &s = link->stats;
        printf("i2slink csim %s TX GPIO%d: writes %u reads new %u timeout %u no_lock %u stalled %u "
               "crc %u | windows %u stale %u torn %u skipped %u jumps %u framing %u locks %u "
               "(fallback %u) hints %u peeks %u\n",
               link->config.clock == I2SLINK_CLOCK_SOURCE ? "source" : "follower",
               link->config.tx_gpio, (unsigned)s.writes, (unsigned)s.reads_new,
               (unsigned)s.reads_timeout, (unsigned)s.reads_no_lock, (unsigned)s.reads_stalled,
               (unsigned)s.reads_crc, (unsigned)s.windows, (unsigned)s.stale, (unsigned)s.torn,
               (unsigned)s.skipped, (unsigned)s.jumps, (unsigned)s.framing, (unsigned)s.locks,
               (unsigned)s.locks_fallback, (unsigned)s.hints_true, (unsigned)s.peeks);
    }
}

// from's TX cycle as to's RX DMA would store it: each 16-bit sample goes
// out MSB first (wire byte k is memory byte k ^ 1 on both ends), and the
// received bit stream is the sent one delayed by rxShiftBits.
void shiftInto(const i2slink_t *from, const i2slink_t *to, uint8_t *ring)
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
        ring[q ^ 1U] = received;
    }
}

// The start of the first pass (of the cycle to's RX hears) at or after t.
uint64_t passAtOrAfter(const i2slink_t *to, uint64_t t)
{
    if (t <= to->rxPhase) {
        return to->rxPhase;
    }
    const uint64_t k = (t - to->rxPhase + kCycle - 1U) / kCycle;
    return to->rxPhase + k * kCycle;
}

// A pending write whose pass has ended goes into the ring.
void deliver(i2slink_t *link)
{
    if (link->pending && now() >= link->pendingAt) {
        memcpy(link->rxRing, link->pendingRing, N);
        link->pending = false;
        ++link->rxVersion;
    }
}

// from has written: the version goes out with the first pass whose region
// starts after now, and is in to's ring when that pass ends. A version
// still on its way is replaced: lost either way, and torn on the wire if
// its pass had begun sending the region.
void transmit(const i2slink_t *from, i2slink_t *to)
{
    deliver(to);
    const uint64_t t = now();
    if (to->pending && t >= to->pendingAt - kCycle + kRegionStart) {
        ++to->stats.torn;
    }
    const uint64_t pass = passAtOrAfter(to, t > kRegionStart ? t - kRegionStart + 1U : 0U);
    shiftInto(from, to, to->pendingRing);
    to->pending = true;
    to->pendingAt = pass + kCycle;
}

// The T byte the ring holds under the current lock (garbage under a wrong
// one, which is what makes a stale lock show).
uint8_t tByte(const i2slink_t *link)
{
    return (uint8_t)wire_decode_u32(link->rxRing, link->phase, link->lane, link->marker, OFF_T);
}

// After a clean copy: T as copied.
void tSave(i2slink_t *link)
{
    link->tSnap = tByte(link);
    link->armedAt = now();
}

// A new lock (found or planted): hint() true from the next pass on.
void tArmNew(i2slink_t *link)
{
    link->tSnap = (uint8_t)~tByte(link);
    link->armedAt = now();
}

void connect(i2slink_t *a, i2slink_t *b)
{
    a->peer = b;
    b->peer = a;
    // A new connection, a new bit alignment and cycle phase each way:
    // readers must relock.
    a->rxShiftBits = rng() % (8U * N);
    b->rxShiftBits = rng() % (8U * N);
    a->rxPhase = now() + rng() % kCycle;
    b->rxPhase = now() + rng() % kCycle;
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

// read() falls back to the exhaustive search; peek() doesn't (allowFull).
bool search(i2slink_t *link, bool allowFull)
{
    uint8_t phase, lane;
    uint16_t marker;
    bool fallback = false;
    const bool found = wire_search(link->rxRing, link->aligned, link->rxTag, link->phase,
                                   link->lane, &phase, &lane, &marker, allowFull, &fallback);
    // The search's time: the full one runs when the anchored one fails.
    csim_wait_until(now() + kSearchAnchored + (allowFull && (!found || fallback) ? kSearchFull : 0U));
    if (!found) {
        return false;
    }
    if (fallback) {
        ++link->stats.locks_fallback;
    }
    link->phase = phase;
    link->lane = lane;
    link->marker = marker;
    link->locked = true;
    ++link->stats.locks;
    tArmNew(link);
    return true;
}

}  // namespace

void csim_i2slink_wire(int gpio_a, int gpio_b)
{
    wires.push_back({gpio_a, gpio_b});
}

extern "C" esp_err_t i2slink_init(i2slink_t **out, const i2slink_config_t *config)
{
    if (!out || !config || (config->i2s_port != 0 && config->i2s_port != 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    runs_init();
    static bool reportRegistered = false;
    if (!reportRegistered) {
        reportRegistered = true;
        atexit(reportLinks);
    }
    i2slink_t *link = new i2slink_t;
    link->config = *config;
    const bool source = config->clock == I2SLINK_CLOCK_SOURCE;
    wire_cycle_init(link->txCycle, source ? TAG_SOURCE : TAG_FOLLOWER);
    memcpy(link->rxTag, source != config->loopback ? TAG_FOLLOWER : TAG_SOURCE, 4U);
    memset(link->rxRing, 0x5C, N);
    link->txSequence = rng();
    link->sync_timeout = (config->sync_timeout_us ? uint64_t(config->sync_timeout_us) * kTicksPerUs
                                                  : 4U * kCycle);
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
    if (i2slink_t *peer = link->peer; peer && peer != link) {
        peer->peer = nullptr;
        peer->pending = false;
        if (peer->config.clock == I2SLINK_CLOCK_SOURCE) {
            // Its RX DMA keeps running on its own clock, hearing an idle line.
            memset(peer->rxRing, 0, N);
            ++peer->rxVersion;
        }
        // A follower's RX DMA stops with the clock: STALLED from now on.
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

// A follower with no source running: its RX DMA has no clock. (A clock
// source with no peer clocks its own RX, which hears an idle line or noise:
// NO_LOCK.)
static bool noClock(const i2slink_t *link)
{
    return !link->peer && link->config.clock == I2SLINK_CLOCK_FOLLOWER;
}

// The beam window at or after now: the reader's VSYNC, which opens as each
// pass ends.
static uint64_t windowAtOrAfter(const i2slink_t *link)
{
    const uint64_t t = now();
    const uint64_t next = passAtOrAfter(link, t);
    if (next != t && next >= link->rxPhase + kCycle && t < next - kCycle + kVsync) {
        return t;   // inside the window that opened at next - kCycle
    }
    return next;
}

static i2slink_status_t readOnce(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                                 uint32_t *sequence, bool canSearch);

extern "C" i2slink_status_t i2slink_read(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                                         uint32_t *sequence, uint32_t wait_us)
{
    const uint64_t start = now();
    const uint64_t deadline =
        wait_us == I2SLINK_WAIT_FOREVER ? UINT64_MAX : start + uint64_t(wait_us) * kTicksPerUs;
    uint64_t faultSince = start;
    for (;;) {
        if (noClock(link)) {
            // No RX DMA progress: STALLED after the link's sync timeout (the
            // client's wait doesn't stretch a fault).
            csim_wait_until(faultSince + link->sync_timeout);
            if (!noClock(link)) {
                continue;
            }
            return finish(link, I2SLINK_STALLED);
        }
        deliver(link);
        i2slink_status_t status = I2SLINK_NO_LOCK;
        link->lockedAtCopy = false;
        if (link->locked || search(link, true)) {
            link->lockedAtCopy = true;
            // Copy only in the beam window (after a search too: it doesn't
            // stop the DMA, so it's waited for afresh).
            csim_wait_until(windowAtOrAfter(link));
            deliver(link);
            status = readOnce(link, payload, sequence, false);
            if (status == I2SLINK_NEW) {
                return finish(link, status);
            }
        }
        const uint64_t t = now();
        if (status == I2SLINK_TIMEOUT) {
            faultSince = t;   // a clean copy: the link is healthy
            if (t >= deadline) {
                return finish(link, I2SLINK_TIMEOUT);
            }
        } else if (t >= faultSince + link->sync_timeout) {
            return finish(link, status);
        } else if (status == I2SLINK_NO_LOCK && link->lockedAtCopy) {
            continue;   // the copy found framing broken: search now
        }
        // Next window: this one is spent.
        csim_wait_until(passAtOrAfter(link, t + 1U));
    }
}

// One look, now, as the hardware's: no beam wait; searches and live
// framing checks at most once per half cycle; a copy only when T has
// changed (writes land whole here, so no torn copies).
extern "C" i2slink_status_t i2slink_peek(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                                         uint32_t *sequence)
{
    ++link->stats.peeks;
    if (noClock(link)) {
        return I2SLINK_NO_LOCK;
    }
    deliver(link);
    const uint64_t t = now();
    if (!link->locked) {
        if (link->peekSearched && t - link->lastPeekSearch < kCycle / 2U) {
            return I2SLINK_NO_LOCK;
        }
        link->peekSearched = true;
        link->lastPeekSearch = t;
        if (!search(link, false)) {
            return finish(link, I2SLINK_NO_LOCK);
        }
    }
    if (tByte(link) == link->tSnap) {
        if (t - link->lastPeekFraming >= kCycle / 2U) {
            link->lastPeekFraming = t;
            if (!wire_framing_ok(link->rxRing, link->phase, link->lane, link->marker)) {
                ++link->stats.framing;
                link->locked = false;
                return finish(link, I2SLINK_NO_LOCK);
            }
        }
        return I2SLINK_TIMEOUT;
    }
    const i2slink_status_t status = readOnce(link, payload, sequence, false);
    if (status == I2SLINK_NEW || status == I2SLINK_NO_LOCK) {
        return finish(link, status);
    }
    return I2SLINK_TIMEOUT;
}

extern "C" bool i2slink_test_lock(const i2slink_t *link, uint8_t *phase, uint8_t *lane,
                                  uint16_t *marker)
{
    *phase = link->phase;
    *lane = link->lane;
    *marker = link->marker;
    return link->locked;
}

extern "C" void i2slink_test_break_lock(i2slink_t *link, uint32_t seed, bool keep_locked)
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
    link->locked = keep_locked;
    tArmNew(link);   // as the hardware's region_set()
}

extern "C" void i2slink_test_dump(const i2slink_t *link, const char *name)
{
    printf("%s lock: %s phase %u lane %u marker %u (csim: no ring dump)\n", name,
           link->locked ? "locked" : "unlocked", link->phase, link->lane, link->marker);
}

extern "C" void i2slink_test_redeliver(i2slink_t *link)
{
    link->haveRead = false;
    tArmNew(link);
}

extern "C" bool i2slink_hint(i2slink_t *link)
{
    if (!link->locked) {
        ++link->stats.hints_true;
        return true;
    }
    deliver(link);
    // No pass has ended since the last copy or lock: nothing to look at.
    if (now() < passAtOrAfter(link, link->armedAt + 1U)) {
        return false;
    }
    if (tByte(link) != link->tSnap) {
        ++link->stats.hints_true;
        return true;
    }
    return false;
}

// One copy of what the ring holds now: NEW, TIMEOUT (the version last
// returned), NO_LOCK or CRC. Counts the per-copy stats; the caller counts
// the read's result (finish()). Without a lock (or with framing broken),
// read() searches (canSearch); peek() leaves that to its next call.
static i2slink_status_t readOnce(i2slink_t *link, uint8_t payload[I2SLINK_PAYLOAD_BYTES],
                                 uint32_t *sequence, bool canSearch)
{
    if (link->locked && !wire_framing_ok(link->rxRing, link->phase, link->lane, link->marker)) {
        ++link->stats.framing;
        link->locked = false;
    }
    if (!link->locked && (!canSearch || !search(link, true))) {
        return I2SLINK_NO_LOCK;
    }
    ++link->stats.windows;
    const uint32_t h = wire_decode_u32(link->rxRing, link->phase, link->lane, link->marker, OFF_H);
    const uint32_t t = wire_decode_u32(link->rxRing, link->phase, link->lane, link->marker, OFF_T);
    if (h != t) {
        ++link->stats.torn;   // can't happen here: a torn pass is never stored
        return I2SLINK_TIMEOUT;
    }
    if (h == 0U || (link->haveRead && h == link->lastRead)) {
        ++link->stats.stale;
        tSave(link);
        return I2SLINK_TIMEOUT;
    }
    wire_decode_payload(link->rxRing, link->phase, link->lane, link->marker, link->decoded);
    const uint32_t crc = wire_decode_u32(link->rxRing, link->phase, link->lane, link->marker, OFF_CRC);
    if (crc32Of(link->decoded, I2SLINK_PAYLOAD_BYTES) != crc) {
        ++link->stats.crc_errors;
        return I2SLINK_CRC;
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
    tSave(link);
    memcpy(payload, link->decoded, I2SLINK_PAYLOAD_BYTES);
    if (sequence) {
        *sequence = h;
    }
    return I2SLINK_NEW;
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
