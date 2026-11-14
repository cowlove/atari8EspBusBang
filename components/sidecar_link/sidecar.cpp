// Link protocol v2: see sidecar.h and docs/sidecar-link-v2.md.

#include "sidecar.h"
#include "esp_attr.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#ifdef CSIM
#include <zlib.h>
#else
#include "esp_cpu.h"
#include "esp_rom_crc.h"
#endif

// The host side's clock: CCOUNT, which touches no bus. esp_timer_get_time()
// reads the SYSTIMER peripheral (a snapshot write, a poll, three reads),
// and on the host every peripheral access from core 0 can delay the core 1
// bus loop's own GPIO register accesses; i2slink itself times with CCOUNT
// too. csim has no CCOUNT: it counts esp_timer microseconds. The 32-bit
// count wraps after about 17 s at 240 MHz, so timeouts must stay below
// that (or be I2SLINK_WAIT_FOREVER).
namespace {
#ifdef CSIM
constexpr uint32_t kTicksPerUs = 1;
inline uint32_t ticksNow() { return static_cast<uint32_t>(esp_timer_get_time()); }
#else
constexpr uint32_t kTicksPerUs = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
inline IRAM_ATTR uint32_t ticksNow() { return esp_cpu_get_cycle_count(); }
#endif

// Microseconds left of timeoutUs since start (0 when it has run out);
// I2SLINK_WAIT_FOREVER stays forever.
inline IRAM_ATTR uint32_t remainingUs(uint32_t start, uint32_t timeoutUs)
{
    if (timeoutUs == I2SLINK_WAIT_FOREVER) {
        return I2SLINK_WAIT_FOREVER;
    }
    const uint32_t elapsedUs = (ticksNow() - start) / kTicksPerUs;
    return elapsedUs < timeoutUs ? timeoutUs - elapsedUs : 0;
}

constexpr size_t kLinkBytes = sizeof(SidecarLinkHeader);

inline IRAM_ATTR SidecarLinkHeader *linkOf(uint8_t *frame)
{
    return reinterpret_cast<SidecarLinkHeader *>(frame);
}
inline IRAM_ATTR SidecarHeader *headerOf(uint8_t *frame)
{
    return reinterpret_cast<SidecarHeader *>(frame + kLinkBytes);
}

// How long a side answering a command waits for the peer to ack its
// previous frame. The peer acks as it reads, so normally it already has.
constexpr uint32_t kRespondAckUs = 5000;

// The Test object's token (its CRC32), computed once at begin(): the host
// answers ReadVersion for it from the hot path, with no time for 100 KB.
DRAM_ATTR uint32_t testObjectVersion = 0;
}  // namespace

uint32_t sidecarCrc32(uint32_t crc, const uint8_t *data, size_t length)
{
#ifdef CSIM
    return static_cast<uint32_t>(crc32(crc, data, static_cast<uInt>(length)));
#else
    return esp_rom_crc32_le(crc, data, length);
#endif
}

IRAM_ATTR uint8_t sidecarTestObjectByte(uint32_t offset)
{
    return static_cast<uint8_t>((offset * 31u + 7u) ^ (offset >> 8) ^ (offset >> 16));
}

// ---------------------------------------------------------------------------
// SidecarPeer: the shared engine
// ---------------------------------------------------------------------------

esp_err_t SidecarPeer::begin(const i2slink_config_t &config, SidecarRole role)
{
    if (link_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (testObjectVersion == 0) {
        uint32_t crc = 0;
        uint8_t block[256];
        for (uint32_t offset = 0; offset < kSidecarTestObjectBytes; offset += sizeof(block)) {
            const uint32_t n = std::min<uint32_t>(sizeof(block), kSidecarTestObjectBytes - offset);
            for (uint32_t i = 0; i < n; ++i) block[i] = sidecarTestObjectByte(offset + i);
            crc = sidecarCrc32(crc, block, n);
        }
        testObjectVersion = crc;
    }
    role_ = role;
    loopback_ = config.loopback;
    primed_ = false;
    havePeer_ = false;
    pendingValid_ = false;
    busy_ = 0;
    // Random starts, so a frame or response left on the wire from before
    // this end restarted can't match a new one.
    nextSequence_ = esp_random();
    txSeq_ = static_cast<uint16_t>(esp_random());
    peerSeq_ = 0;
    peerAck_ = static_cast<uint16_t>(txSeq_ - 1);
    const esp_err_t err = i2slink_init(&link_, &config);
    if (err != ESP_OK) {
        return err;
    }
    // The first frame: Idle (nothing in it needs protecting, so the first
    // real frame needn't wait for its ack).
    memset(tx_, 0, sizeof(tx_));
    SidecarLinkHeader *link = linkOf(tx_);
    link->version = kSidecarProtocolVersion;
    link->mode = static_cast<uint8_t>(SidecarMode::Idle);
    link->frameSeq = txSeq_;
    link->ackSeq = peerSeq_;
    i2slink_write(link_, tx_);
    return ESP_OK;
}

void SidecarPeer::end()
{
    i2slink_deinit(link_);
    link_ = nullptr;
}

IRAM_ATTR bool SidecarPeer::pump(uint32_t waitUs)
{
    // No wait wanted: one look (i2slink_peek(), never waits). Otherwise
    // read(), which waits for news and for the beam window.
    lastStatus_ = waitUs ? i2slink_read(link_, rx_, nullptr, waitUs) : i2slink_peek(link_, rx_, nullptr);
    if (lastStatus_ != I2SLINK_NEW) {
        return false;
    }
    const SidecarLinkHeader *link = linkOf(rx_);
    if (link->version != kSidecarProtocolVersion) {
        ++versionMismatches_;   // an old (v1) or newer peer: not ours to answer
        return false;
    }
    peerAck_ = link->ackSeq;
    if (havePeer_ && link->frameSeq == peerSeq_) {
        return false;   // the same frame, rewritten with a new ack
    }
    havePeer_ = true;
    peerSeq_ = link->frameSeq;
    // Ack it at once: a rewrite of our current frame with the new ackSeq
    // (not a new frame, so always allowed).
    linkOf(tx_)->ackSeq = peerSeq_;
    i2slink_write(link_, tx_);
    return true;
}

// A command read while waiting: the host ignores it (it has priority); the
// Sidecar keeps it in pending_ to serve, and its own command is abandoned.
// True if this side must yield.
IRAM_ATTR bool SidecarPeer::noteIncomingCommand()
{
    if (role_ != SidecarRole::Subordinate) {
        return false;
    }
    memcpy(pending_, rx_, sizeof(pending_));
    pendingValid_ = true;
    ++preempted_;
    return true;
}

IRAM_ATTR bool SidecarPeer::waitForAck(uint32_t timeoutUs)
{
    if (linkOf(tx_)->mode == static_cast<uint8_t>(SidecarMode::Idle)) {
        return true;   // nothing to protect
    }
    const uint32_t start = ticksNow();
    while (peerAck_ != txSeq_) {
        const uint32_t remaining = remainingUs(start, timeoutUs);
        if (remaining == 0) {
            return false;
        }
        if (pump(remaining) && !loopback_ &&
            linkOf(rx_)->mode == static_cast<uint8_t>(SidecarMode::Command) &&
            noteIncomingCommand()) {
            return false;   // yield
        }
    }
    return true;
}

IRAM_ATTR void SidecarPeer::writeNewFrame(SidecarMode mode)
{
    SidecarLinkHeader *link = linkOf(tx_);
    link->version = kSidecarProtocolVersion;
    link->mode = static_cast<uint8_t>(mode);
    link->frameSeq = ++txSeq_;
    link->ackSeq = peerSeq_;
    i2slink_write(link_, tx_);
}

IRAM_ATTR SidecarResult SidecarPeer::transact(SidecarCommand command, void *message, void *response,
                                              uint32_t timeoutUs)
{
    if (!link_) {
        lastStatus_ = I2SLINK_NO_LOCK;
        return SidecarResult::LinkFault;
    }
    BusyScope busy(busy_);
    if (pendingValid_) {
        return SidecarResult::Preempted;   // serve the peer's command first
    }
    uint8_t *frame = static_cast<uint8_t *>(message);
    SidecarHeader *header = headerOf(frame);
    header->command = static_cast<uint8_t>(command);
    header->sequence = nextSequence_++;
    header->status = 0;

    const uint32_t start = ticksNow();
    if (!waitForAck(timeoutUs)) {
        if (pendingValid_) {
            return SidecarResult::Preempted;
        }
        lastStatus_ = lastStatus_ == I2SLINK_NEW ? I2SLINK_TIMEOUT : lastStatus_;
        return lastStatus_ == I2SLINK_TIMEOUT ? SidecarResult::Timeout : SidecarResult::LinkFault;
    }
    // The message part (header, body, data): the link header is ours.
    memcpy(tx_ + kLinkBytes, frame + kLinkBytes, I2SLINK_PAYLOAD_BYTES - kLinkBytes);
    writeNewFrame(SidecarMode::Command);

    // The last link fault seen, if any: reported if time runs out.
    i2slink_status_t fault = I2SLINK_NEW;
    for (;;) {
        const uint32_t remaining = remainingUs(start, timeoutUs);
        if (remaining == 0) {
            lastStatus_ = fault != I2SLINK_NEW ? fault : I2SLINK_TIMEOUT;
            return fault != I2SLINK_NEW ? SidecarResult::LinkFault : SidecarResult::Timeout;
        }
        // The wait happens in i2slink_read(), paced and memory-only.
        if (!pump(remaining)) {
            if (lastStatus_ == I2SLINK_TIMEOUT) {
                if (fault != I2SLINK_NEW) {
                    lastStatus_ = fault;   // timed out after a fault: report the fault
                    return SidecarResult::LinkFault;
                }
                return SidecarResult::Timeout;
            }
            if (lastStatus_ != I2SLINK_NEW) {
                // TORN, CRC, NO_LOCK or STALLED, for sync_timeout_us. The
                // link keeps resending, and stale replies are ignored by
                // sequence, so a fault that clears in time costs nothing.
                fault = lastStatus_;
            }
            continue;   // a fault, an ack-only rewrite or another version
        }
        const uint8_t mode = linkOf(rx_)->mode;
        const SidecarHeader *reply = headerOf(rx_);
        const bool ours = reply->command == header->command && reply->sequence == header->sequence;
        if (mode == static_cast<uint8_t>(SidecarMode::Response) && ours) {
            memcpy(response, rx_, I2SLINK_PAYLOAD_BYTES);
            return SidecarResult::Ok;
        }
        if (mode == static_cast<uint8_t>(SidecarMode::Command)) {
            if (loopback_ && ours) {
                memcpy(response, rx_, I2SLINK_PAYLOAD_BYTES);   // our own command, read back
                return SidecarResult::Ok;
            }
            if (!loopback_ && noteIncomingCommand()) {
                return SidecarResult::Preempted;   // our command was never run: retry later
            }
        }
        // A response to some other (earlier) command, an Idle frame, or (the
        // host) the Sidecar's command: keep waiting.
    }
}

SidecarResult SidecarPeer::readVersion(SidecarObject object, uint32_t *size, uint32_t *version,
                                       SidecarReadStatus *status, uint32_t timeoutUs)
{
    ReadVersionCommand command = {};
    command.body.object = static_cast<uint8_t>(object);
    ReadVersionResponse response;
    const SidecarResult result = transact(SidecarCommand::ReadVersion, &command, &response, timeoutUs);
    if (result == SidecarResult::Ok) {
        *size = response.body.size;
        *version = response.body.version;
        *status = static_cast<SidecarReadStatus>(response.header.status);
    }
    return result;
}

SidecarResult SidecarPeer::readChunk(SidecarObject object, uint32_t offset, uint8_t *buffer,
                                     uint16_t *length, SidecarReadStatus *status, uint32_t timeoutUs)
{
    ReadChunkCommand command = {};
    command.body.object = static_cast<uint8_t>(object);
    command.body.offset = offset;
    ReadChunkResponse response;
    const SidecarResult result = transact(SidecarCommand::ReadChunk, &command, &response, timeoutUs);
    if (result == SidecarResult::Ok) {
        *status = static_cast<SidecarReadStatus>(response.header.status);
        *length = std::min<uint16_t>(response.body.length, sizeof(response.data));
        memcpy(buffer, response.data, *length);
    }
    return result;
}

// pending_ holds a command; work_ starts as its copy and becomes the
// response.
IRAM_ATTR bool SidecarPeer::handleBuiltin(const SidecarHeader &header)
{
    switch (static_cast<SidecarCommand>(header.command)) {
    case SidecarCommand::Nop:
        return true;   // the echo is the answer
    case SidecarCommand::ReadVersion: {
        auto &response = *reinterpret_cast<ReadVersionResponse *>(work_);
        if (response.body.object != static_cast<uint8_t>(SidecarObject::Test)) {
            return false;   // not built in
        }
        response.body.size = kSidecarTestObjectBytes;
        response.body.version = testObjectVersion;
        response.header.status = static_cast<uint8_t>(SidecarReadStatus::Ok);
        return true;
    }
    case SidecarCommand::ReadChunk: {
        auto &response = *reinterpret_cast<ReadChunkResponse *>(work_);
        if (response.body.object != static_cast<uint8_t>(SidecarObject::Test)) {
            return false;
        }
        const uint32_t offset = response.body.offset;
        const uint32_t left = offset < kSidecarTestObjectBytes ? kSidecarTestObjectBytes - offset : 0;
        const uint32_t length = std::min<uint32_t>(left, sizeof(response.data));
        for (uint32_t i = 0; i < length; ++i) response.data[i] = sidecarTestObjectByte(offset + i);
        response.body.length = static_cast<uint16_t>(length);
        response.header.status = static_cast<uint8_t>(SidecarReadStatus::Ok);
        return true;
    }
    default:
        return false;
    }
}

// Answer pending_: the response is built in work_ (a copy of the command,
// status 0) by handler, which returns false for no response; then, once the
// peer has acked our previous frame, it becomes our new frame.
template <typename Handler>
IRAM_ATTR bool SidecarPeer::respond(Handler &&handler)
{
    pendingValid_ = false;
    if (!primed_) {
        primed_ = true;   // the first command since begin(): possibly run before a restart
        return false;
    }
    memcpy(work_, pending_, sizeof(work_));
    SidecarHeader *header = headerOf(work_);
    const SidecarHeader command = *headerOf(pending_);
    header->status = 0;
    if (!handler(command)) {
        return false;   // unknown command: no response; the sender times out
    }
    // Always echo the header; keep the status the handler set.
    header->command = command.command;
    header->sequence = command.sequence;
    header->flags = command.flags;
    if (!waitForAck(kRespondAckUs)) {
        return false;   // the peer never acked our last frame: it'll retry
    }
    memcpy(tx_ + kLinkBytes, work_ + kLinkBytes, I2SLINK_PAYLOAD_BYTES - kLinkBytes);
    writeNewFrame(SidecarMode::Response);
    ++served_;
    return true;
}

IRAM_ATTR bool SidecarPeer::readyToRespond()
{
    const auto acked = [this]() __attribute__((always_inline)) {
        return linkOf(tx_)->mode == static_cast<uint8_t>(SidecarMode::Idle) || peerAck_ == txSeq_;
    };
    if (acked()) {
        return true;
    }
    // The ack comes in the peer's next frame (an ack-only rewrite): look
    // once. pending_ keeps the command meanwhile.
    pump(0);
    return acked();
}

// Read a new frame and keep it if it's a command; true if pending_ holds
// one now. One i2slink_peek(): a couple of loads with nothing new, never a wait.
IRAM_ATTR bool SidecarPeer::takeCommand()
{
    if (pendingValid_) {
        return true;
    }
    if (!pump(0)) {
        return false;
    }
    if (linkOf(rx_)->mode != static_cast<uint8_t>(SidecarMode::Command)) {
        return false;
    }
    memcpy(pending_, rx_, sizeof(pending_));
    pendingValid_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// Sidecar (host side)
// ---------------------------------------------------------------------------

esp_err_t Sidecar::begin(const i2slink_config_t &config)
{
    present_ = false;
    return SidecarPeer::begin(config, SidecarRole::Priority);
}

// Everything from probe() down runs from IRAM, never flash: on the host
// it polls on core 0 beside the core 1 bus loop.
IRAM_ATTR bool Sidecar::probe(uint32_t timeoutUs, uint32_t retryUs)
{
    const uint32_t start = ticksNow();
    for (;;) {
        const uint32_t remaining = remainingUs(start, timeoutUs);
        if (remaining == 0) {
            present_ = false;
            return false;
        }
        // Each try is a new frame, so a Sidecar that dropped the previous
        // one (its first command after a restart) sees a new command.
        NopCommand command = {};
        NopResponse response;
        if (nop(command, response, std::min(retryUs, remaining)) == SidecarResult::Ok) {
            present_ = true;
            return true;
        }
    }
}

IRAM_ATTR SidecarResult Sidecar::nop(NopCommand &command, NopResponse &response, uint32_t timeoutUs)
{
    return transact(SidecarCommand::Nop, &command, &response, timeoutUs);
}

IRAM_ATTR SidecarResult Sidecar::sio(SioCommand &command, SioResponse &response, uint32_t timeoutUs)
{
    return transact(SidecarCommand::Sio, &command, &response, timeoutUs);
}

IRAM_ATTR SidecarResult Sidecar::cio(CioCommand &command, CioResponse &response, uint32_t timeoutUs)
{
    return transact(SidecarCommand::Cio, &command, &response, timeoutUs);
}

// poll()'s answers. always_inline: it must land in poll()'s IRAM (the
// hot-call check), not flash.
inline __attribute__((always_inline)) bool Sidecar::handleHost(const SidecarHeader &header)
{
    if (handleBuiltin(header)) {
        return true;
    }
    switch (static_cast<SidecarCommand>(header.command)) {
    case SidecarCommand::ReadVersion:
    case SidecarCommand::ReadChunk: {
        // Core: only in diagnostic mode (pollDiagnostic()); it reads flash.
        const uint8_t object = reinterpret_cast<ReadChunkCommand *>(pending_)->body.object;
        headerOf(work_)->status = static_cast<uint8_t>(
            object == static_cast<uint8_t>(SidecarObject::Core) ? SidecarReadStatus::Unavailable
                                                                 : SidecarReadStatus::NoObject);
        return true;
    }
    default:
        return false;   // the host serves nothing else: the Sidecar times out
    }
}

IRAM_ATTR bool Sidecar::poll()
{
    if (!link_ || loopback_) {
        return false;   // loopback: no peer, only our own frames
    }
    BusyScope busy(busy_);
    // A true poll: nothing here waits. takeCommand() and readyToRespond()
    // look with i2slink_peek() (one look; no beam-window wait; without a
    // lock, one ring search at most); a response whose turn hasn't come (our
    // last frame not yet acked) waits for a later poll, not in respond().
    if (!takeCommand() || !readyToRespond()) {
        return false;
    }
    // always_inline: the handler must land in poll()'s IRAM, not flash.
    return respond([this](const SidecarHeader &header) __attribute__((always_inline)) {
        return handleHost(header);
    });
}

bool Sidecar::pollDiagnostic()
{
    if (!link_ || loopback_) {
        return false;
    }
    BusyScope busy(busy_);
    if (!takeCommand() || !readyToRespond()) {
        return false;
    }
    return respond([this](const SidecarHeader &header) {
        const auto command = static_cast<SidecarCommand>(header.command);
        const bool core = (command == SidecarCommand::ReadVersion || command == SidecarCommand::ReadChunk) &&
                          reinterpret_cast<ReadChunkCommand *>(pending_)->body.object ==
                              static_cast<uint8_t>(SidecarObject::Core);
        if (!core) {
            return handleHost(header);
        }
        if (!coreReader_ || coreSize_ == 0) {
            headerOf(work_)->status = static_cast<uint8_t>(SidecarReadStatus::Unavailable);
            return true;
        }
        if (command == SidecarCommand::ReadVersion) {
            auto &response = *reinterpret_cast<ReadVersionResponse *>(work_);
            response.body.size = coreSize_;
            response.body.version = coreVersion_;
            response.header.status = static_cast<uint8_t>(SidecarReadStatus::Ok);
            return true;
        }
        auto &response = *reinterpret_cast<ReadChunkResponse *>(work_);
        const uint32_t offset = response.body.offset;
        const uint32_t left = offset < coreSize_ ? coreSize_ - offset : 0;
        const uint32_t length = std::min<uint32_t>(left, sizeof(response.data));
        if (length && !coreReader_(offset, response.data, length)) {
            response.header.status = static_cast<uint8_t>(SidecarReadStatus::IoError);
            return true;
        }
        response.body.length = static_cast<uint16_t>(length);
        response.header.status = static_cast<uint8_t>(SidecarReadStatus::Ok);
        return true;
    });
}

bool Sidecar::mount(int unitId, const std::string &diskSpec, uint32_t timeoutUs)
{
    if (!present_) {
        printf("Sidecar: D%d: '%s': no Sidecar present\n", unitId, diskSpec.c_str());
        return false;
    }
    MountCommand command = {};
    if (unitId < 1 || unitId > 255 || diskSpec.size() >= sizeof(command.data)) {
        printf("Sidecar: D%d: '%s': bad unit or diskSpec too long (max %u bytes)\n", unitId,
               diskSpec.c_str(), (unsigned)sizeof(command.data) - 1);
        return false;
    }
    command.body.unit = static_cast<uint8_t>(unitId);
    memcpy(command.data, diskSpec.c_str(), diskSpec.size() + 1);
    MountResponse response;
    const SidecarResult result = transact(SidecarCommand::Mount, &command, &response, timeoutUs);
    if (result != SidecarResult::Ok) {
        printf("Sidecar: D%d: '%s': mount %s (i2slink status %d)\n", unitId, diskSpec.c_str(),
               result == SidecarResult::Timeout ? "timed out" : "link fault", (int)lastStatus_);
        return false;
    }
    if (loopback_) {
        printf("Sidecar: D%d: '%s': loopback, the mount command came back as its echo\n", unitId,
               diskSpec.c_str());
        return false;
    }
    switch (static_cast<SidecarMountStatus>(response.header.status)) {
    case SidecarMountStatus::Mounted:
        printf("Sidecar: D%d: '%s' mounted on the Sidecar\n", unitId, diskSpec.c_str());
        return true;
    case SidecarMountStatus::Failed:
        printf("Sidecar: D%d: '%s': the Sidecar could not mount it\n", unitId, diskSpec.c_str());
        return false;
    default:
        printf("Sidecar: D%d: '%s': mount rejected (status %u)\n", unitId, diskSpec.c_str(),
               (unsigned)response.header.status);
        return false;
    }
}

IRAM_ATTR SidecarResult Sidecar::loopbackCheck(uint32_t timeoutUs)
{
    // A body that differs every call (xorshift32 seeded from the sequence
    // number this command is about to get), so a stale echo can't pass.
    // Not esp_random(): that reads the RNG peripheral.
    NopCommand command;
    NopResponse response;
    uint32_t state = nextSequence_ * 2654435761u | 1u;
    uint8_t *bytes = reinterpret_cast<uint8_t *>(&command);
    for (size_t index = kLinkBytes + sizeof(SidecarHeader); index < sizeof(command); ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[index] = static_cast<uint8_t>(state);
    }
    command.header.flags = 0;
    const SidecarResult result = nop(command, response, timeoutUs);
    if (result != SidecarResult::Ok) {
        return result;
    }
    // From the message header on (the link headers differ). A plain loop:
    // memcmp() isn't on the hot-call check's ROM allowlist.
    const uint8_t *sent = bytes;
    const uint8_t *echoed = reinterpret_cast<const uint8_t *>(&response);
    for (size_t index = kLinkBytes; index < sizeof(command); ++index) {
        if (sent[index] != echoed[index]) {
            return SidecarResult::Mismatch;
        }
    }
    return SidecarResult::Ok;
}

// ---------------------------------------------------------------------------
// SidecarService (Sidecar side)
// ---------------------------------------------------------------------------

// While the link is down (no host yet, no clock), hint() is always true and
// each read() spends its sync timeout searching; back off so service() stays
// cheap for the outer loop.
static constexpr int64_t kFaultBackoffUs = 10000;

esp_err_t SidecarService::begin(const i2slink_config_t &config)
{
    retryAfterUs_ = 0;
    return SidecarPeer::begin(config, SidecarRole::Subordinate);
}

bool SidecarService::service()
{
    if (!link_) {
        return false;
    }
    BusyScope busy(busy_);
    if (!pendingValid_) {
        if (retryAfterUs_ && esp_timer_get_time() < retryAfterUs_) {
            return false;
        }
        if (!takeCommand()) {
            // TIMEOUT or NEW (nothing for us): fine. Anything else is a link
            // fault: try again later.
            const bool fault = lastStatus_ != I2SLINK_TIMEOUT && lastStatus_ != I2SLINK_NEW;
            retryAfterUs_ = fault ? esp_timer_get_time() + kFaultBackoffUs : 0;
            return false;
        }
        retryAfterUs_ = 0;
    }
    return respond([this](const SidecarHeader &header) __attribute__((always_inline)) {
        switch (static_cast<SidecarCommand>(header.command)) {
        case SidecarCommand::Nop:
            handleNop();
            return true;   // the echo is the answer
        case SidecarCommand::Sio:
            handleSio(*reinterpret_cast<const SioCommand *>(pending_),
                      *reinterpret_cast<SioResponse *>(work_));
            return true;
        case SidecarCommand::Cio:
            handleCio(*reinterpret_cast<const CioCommand *>(pending_),
                      *reinterpret_cast<CioResponse *>(work_));
            return true;
        case SidecarCommand::BootStatus:
            handleBootStatus(*reinterpret_cast<const BootStatusCommand *>(pending_),
                             *reinterpret_cast<BootStatusResponse *>(work_));
            return true;
        case SidecarCommand::Mount:
            handleMount(*reinterpret_cast<const MountCommand *>(pending_),
                        *reinterpret_cast<MountResponse *>(work_));
            return true;
        case SidecarCommand::ReadVersion:
            if (!handleBuiltin(header)) {
                handleReadVersion(*reinterpret_cast<const ReadVersionCommand *>(pending_),
                                  *reinterpret_cast<ReadVersionResponse *>(work_));
            }
            return true;
        case SidecarCommand::ReadChunk:
            if (!handleBuiltin(header)) {
                handleReadChunk(*reinterpret_cast<const ReadChunkCommand *>(pending_),
                                *reinterpret_cast<ReadChunkResponse *>(work_));
            }
            return true;
        default:
            return false;   // unknown command: no response; the host times out
        }
    });
}

void SidecarService::handleReadVersion(const ReadVersionCommand &, ReadVersionResponse &response)
{
    response.header.status = static_cast<uint8_t>(SidecarReadStatus::NoObject);
}

void SidecarService::handleReadChunk(const ReadChunkCommand &, ReadChunkResponse &response)
{
    response.header.status = static_cast<uint8_t>(SidecarReadStatus::NoObject);
}
