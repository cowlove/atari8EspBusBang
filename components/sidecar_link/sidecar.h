#pragma once

// Sidecar: an optional second ESP32 that works as a general coprocessor for
// the host (this board): disk/SIO and CIO IO for now; later perhaps video,
// sound, LEDs and bus monitoring the host has no pins or time for.
//
// The host and the Sidecar talk over i2slink (the hands-free I2S DMA link,
// i2slink.h) with link protocol v2 (docs/sidecar-link-v2.md): symmetric
// frames, either side may send a command, the host has priority.
//
// What the protocol relies on from i2slink:
//   - each direction carries the latest value, not a queue;
//   - a value stays on the wire until its writer writes again, so one write
//     is enough: the hardware resends it every wire cycle;
//   - read() returns each version at most once (NEW is always new).
//
// Frames. Both directions carry the same frame: a link header (protocol
// version, mode, this side's frame sequence, the last frame sequence seen
// from the peer) and one message: a command or a response (mode says which;
// a frame is never both), with its header, body and data.
//
// Rules (docs/sidecar-link-v2.md):
//   1. No new frame (a new frameSeq) until the peer has acked the current
//      one (its ackSeq equals our frameSeq). Acking is always allowed: a
//      rewrite with only a new ackSeq is not a new frame. Each side acks
//      every new peer frame as soon as it reads it.
//   2. The receiver copies what it needs out of a frame when it reads it.
//   3. One outstanding command per side.
//   4. The host has priority: waiting for a response, it ignores commands.
//   5. The Sidecar yields: waiting for a response and seeing a command, it
//      abandons its own (transact() returns Preempted) and serves the
//      command; the caller retries later. Rule 1 means an abandoned command
//      was never run.
//   6. An idle side serves commands (Sidecar::poll(), SidecarService::
//      service()).
//   7. Restarts: a side that restarts may find the peer's last command on
//      the wire, possibly already run, so it drops the first command it
//      sees after begin(). A Nop probe re-establishes the link.
//
// Single-threaded: all calls on one side from one task (i2slink's hint/read/
// write all run in the caller's context).

#include <cstddef>
#include <cstdint>
#include <string>

#include "i2slink.h"
#include "atariStructs.h"   // AtariDCB, AtariIOCB

static constexpr uint8_t kSidecarProtocolVersion = 2;

enum class SidecarMode : uint8_t {
    Idle = 0,       // nothing yet (just after begin())
    Command = 1,
    Response = 2,
};

struct __attribute__((packed)) SidecarLinkHeader {
    uint8_t version;     // kSidecarProtocolVersion; frames of another version are ignored
    uint8_t mode;        // SidecarMode
    uint16_t frameSeq;   // this side's frame sequence: bumped for each new frame
    uint16_t ackSeq;     // the last frameSeq seen from the peer
};

enum class SidecarCommand : uint8_t {
    Nop = 0,          // body: none; answered with an echo (probe(), presence)
    Sio = 1,          // body: AtariDCB, then optional raw data for the DCB
    Cio = 2,          // body: AtariIOCB, then optional raw data for the IOCB
    Mount = 3,        // body: SidecarMountBody, then the diskSpec (NUL-terminated)
    ReadVersion = 4,  // body: SidecarReadVersionBody (either direction)
    ReadChunk = 5,    // body: SidecarReadChunkBody, response data: the chunk (either direction)
    // 6: unused (was SetDiagnostic; diagnostic mode now comes in BootStatus's reply)
    BootStatus = 7,     // body: SidecarBootStatusBody (main to Sidecar, once per boot)
};

struct __attribute__((packed)) SidecarHeader {
    uint8_t command;     // SidecarCommand
    uint32_t sequence;   // chosen by the sender; the response echoes it
    uint8_t flags;       // reserved for future expansion; 0 for now
    uint8_t status;      // commands: 0. Responses: set by the handler (0 if it
                         // leaves it alone); its meaning is per command
};

// One whole link payload: link header, message header, the command-specific
// struct, then raw data filling the rest. Sends always carry the full
// payload size.
template <typename Body>
struct __attribute__((packed)) SidecarMessage {
    static constexpr size_t kDataBytes = I2SLINK_PAYLOAD_BYTES - sizeof(SidecarLinkHeader) -
                                         sizeof(SidecarHeader) - sizeof(Body);

    SidecarLinkHeader link;   // filled in by the link layer
    SidecarHeader header;
    Body body;
    uint8_t data[kDataBytes];   // only the bytes the command calls for are meaningful
};

struct __attribute__((packed)) SidecarNopBody {
    uint8_t reserved;   // 0
};

// Mount: the Sidecar mounts data[] (a diskSpec in Sio syntax,
// components/disk_image/sio.h, NUL-terminated) in its drive `unit` with its
// own Sio. The response's header.status is a SidecarMountStatus.
struct __attribute__((packed)) SidecarMountBody {
    uint8_t unit;       // 1 = D1:, as in the DCB's DUNIT
    uint8_t reserved;   // 0
};

enum class SidecarMountStatus : uint8_t {
    Mounted = 0,      // Sio::mount() succeeded (or the diskSpec was empty: unmounted)
    Failed = 1,       // Sio::mount() failed; the Sidecar's log says why
    BadRequest = 2,   // unit out of range, or no NUL in data
};

// Objects a side can offer for reading (ReadVersion/ReadChunk): a constant
// enum, not file names.
enum class SidecarObject : uint8_t {
    Firmware = 1,   // the Sidecar: the main app image to install (main OTA)
    Core = 2,       // the host: its last core dump
    Test = 3,       // both: a fixed pattern, for checking fetches
};

// ReadVersion / ReadChunk response status (header.status).
enum class SidecarReadStatus : uint8_t {
    Ok = 0,
    NoObject = 1,      // this side doesn't offer that object
    Unavailable = 2,   // it does, but there's nothing now
    IoError = 3,
};

// ReadVersion: the command sets object; the response fills size and
// version, a token that changes whenever the content does (a CRC32 of the
// content). A reader fetches, then reads the version again: the same token
// means it got one consistent copy.
struct __attribute__((packed)) SidecarReadVersionBody {
    uint8_t object;   // SidecarObject
    uint8_t reserved[3];
    uint32_t size;
    uint32_t version;
};

// ReadChunk: the command sets object and offset; the response returns up to
// kDataBytes of content from offset in data, and length. A zero length is
// the end (EOF); the last chunk may be short. No state on the server: the
// same object and offset give the same bytes.
struct __attribute__((packed)) SidecarReadChunkBody {
    uint8_t object;   // SidecarObject
    uint8_t reserved;
    uint16_t length;  // response: bytes of data
    uint32_t offset;
};

// Diagnostic mode, for anything that reads or writes main's flash (core
// dump fetch, firmware update): main never attaches the bus (a running
// Atari crashes; the planned hardware also holds its reset low) and only
// serves the link until reset. Only the Sidecar's reply to BootStatus can
// ask for it (body.diagnostic), so it's decided at boot, before core 1
// starts; the bmon loop never polls the link. To force it on a running
// main, the Sidecar resets main and answers the next boot report. Any flag
// implies Diagnostic.
enum class SidecarDiagnosticFlag : uint8_t {
    Diagnostic = 0x01,      // don't attach the bus; serve the link until reset
    FetchFirmware = 0x02,   // then pull the Sidecar's Firmware object and install it (OTA)
    Panic = 0x04,           // then abort(): a deliberate crash, to test core dumps
};

// BootStatus (main to Sidecar): sent once per boot, from SidecarHost::
// begin() after the probe, before core 1 starts (main can still read flash
// freely there). The Sidecar decides at once, in its handler, and replies
// with the diagnostic flags main must enter (0: boot normally). Its rule
// for now: a core dump it hasn't seen means main just crashed, so
// Diagnostic, then it fetches the core; plus any flags staged on its
// console for the next boot.
// Versions are the first 4 bytes of a SHA-256, little endian; 0 = none.
enum class SidecarBootFlag : uint8_t {
    CorePartition = 0x01,   // main has a core dump partition (the core dump table)
    CorePresent = 0x02,     // and a core dump is in it
    OtaSlot = 0x04,         // main has an ota_1 partition (the OTA table)
};
struct __attribute__((packed)) SidecarBootStatusBody {
    uint8_t resetReason;   // esp_reset_reason_t (ESP_RST_PANIC = 4, ...)
    uint8_t flags;         // SidecarBootFlag bits
    uint8_t diagnostic;    // command: 0; the reply: SidecarDiagnosticFlag bits main must enter
    uint8_t reserved;
    uint32_t coreSize;     // bytes, checksum included; 0 = none
    uint32_t coreVersion;  // the core dump's stored SHA-256 (no rehash): new dump, new token
    uint32_t appVersion;   // the running app's ELF SHA-256
    char git[24];          // the running app's GIT_VERSION, NUL-terminated
};

using NopCommand = SidecarMessage<SidecarNopBody>;
using SioCommand = SidecarMessage<AtariDCB>;    // 279 data bytes: a 256-byte sector fits
using CioCommand = SidecarMessage<AtariIOCB>;   // 275 data bytes; chunk longer buffers
using MountCommand = SidecarMessage<SidecarMountBody>;
using ReadVersionCommand = SidecarMessage<SidecarReadVersionBody>;
using ReadChunkCommand = SidecarMessage<SidecarReadChunkBody>;
using BootStatusCommand = SidecarMessage<SidecarBootStatusBody>;
// The responses mirror the commands.
using NopResponse = SidecarMessage<SidecarNopBody>;
// SIO: the command carries the Atari's DCB as is, plus DBYT bytes of data
// for a write. The response's header.status is 1 if the drive did the
// command (the host sets the 6502's carry) and 0 if not; body.DSTATS is the
// status for the Atari's DCB; data holds what a read returns (4 bytes for
// status, DBYT for the rest).
using SioResponse = SidecarMessage<AtariDCB>;
using CioResponse = SidecarMessage<AtariIOCB>;
using MountResponse = SidecarMessage<SidecarMountBody>;
using ReadVersionResponse = SidecarMessage<SidecarReadVersionBody>;
using ReadChunkResponse = SidecarMessage<SidecarReadChunkBody>;
using BootStatusResponse = SidecarMessage<SidecarBootStatusBody>;

static_assert(sizeof(NopCommand) == I2SLINK_PAYLOAD_BYTES, "Nop message must fill the payload");
static_assert(sizeof(SioCommand) == I2SLINK_PAYLOAD_BYTES, "SIO message must fill the payload");
static_assert(sizeof(CioCommand) == I2SLINK_PAYLOAD_BYTES, "CIO message must fill the payload");
static_assert(sizeof(MountCommand) == I2SLINK_PAYLOAD_BYTES, "Mount message must fill the payload");
static_assert(sizeof(ReadChunkCommand) == I2SLINK_PAYLOAD_BYTES, "ReadChunk message must fill the payload");
static_assert(SioCommand::kDataBytes >= 256, "a 256-byte sector must fit a frame");

enum class SidecarResult {
    Ok,          // response received; it echoes the command's header
    Timeout,     // link healthy, but no matching response in time
    LinkFault,   // the link itself failed (see lastLinkStatus())
    Mismatch,    // loopbackCheck() only: the echo differed from the command
    Preempted,   // the Sidecar only: the host sent a command meanwhile; serve
                 // it (service()) and retry
};

enum class SidecarRole : uint8_t {
    Priority,      // the host: never yields
    Subordinate,   // the Sidecar: yields to a command from the host
};

// The Test object: a fixed pattern both sides serve, for checking fetches.
static constexpr uint32_t kSidecarTestObjectBytes = 100000;
uint8_t sidecarTestObjectByte(uint32_t offset);

// What a fetch() delivers: each chunk in order, then the result.
struct SidecarFetchResult {
    SidecarResult link;          // the last transaction's result
    SidecarReadStatus status;    // the server's status (Ok when link is Ok)
    uint32_t size;               // the object's size (ReadVersion)
    uint32_t version;            // its token (ReadVersion, before)
    uint32_t received;           // bytes delivered to the callback
    uint32_t crc;                // CRC32 of them
    bool consistent;             // all of it (received == size), and the token after equals the one before
    bool crcMatches;             // crc == version: a full check where the token is a CRC32
                                 // (Firmware, Test); Core's token is a SHA-256 prefix instead
};

// The link engine both sides share: frames, sequence numbers and acks, and
// the command/response exchange. Everything here is non-virtual and in IRAM:
// on the host it runs on core 0 beside the core 1 bus loop.
class SidecarPeer {
public:
    // Starts the link (clock role, I2S port and pins in config).
    esp_err_t begin(const i2slink_config_t &config, SidecarRole role);
    void end();

    // Send a command (message: a whole payload; header.command, sequence and
    // status are filled in) and wait up to timeoutUs for its response, into
    // response (a whole payload; untouched unless Ok).
    SidecarResult transact(SidecarCommand command, void *message, void *response, uint32_t timeoutUs);

    // Reading objects from the peer. readVersion() fills size and version;
    // readChunk() up to SidecarMessage<...>::kDataBytes bytes into buffer,
    // with *length the count (0: end). *status is the peer's
    // SidecarReadStatus when the result is Ok.
    SidecarResult readVersion(SidecarObject object, uint32_t *size, uint32_t *version,
                              SidecarReadStatus *status, uint32_t timeoutUs);
    SidecarResult readChunk(SidecarObject object, uint32_t offset, uint8_t *buffer,
                            uint16_t *length, SidecarReadStatus *status, uint32_t timeoutUs);

    i2slink_status_t lastLinkStatus() const { return lastStatus_; }
    i2slink_t *link() const { return link_; }
    bool loopback() const { return loopback_; }

    // Counters, for logs.
    uint32_t preempted() const { return preempted_; }
    uint32_t served() const { return served_; }
    uint32_t versionMismatches() const { return versionMismatches_; }

    // Inside one of this side's calls (transact, poll, service): csim's
    // simulated link runs a side's poll only when it isn't.
    bool busy() const { return busy_ != 0; }

protected:
    struct BusyScope {
        uint8_t &count;
        explicit BusyScope(uint8_t &c) : count(c) { ++count; }
        ~BusyScope() { --count; }
    };

    // Read at most one new frame, waiting up to waitUs. Updates the peer's
    // ack and acks the frame. Returns true if rx_ holds a new frame (a new
    // frameSeq) of our version.
    bool pump(uint32_t waitUs);
    // A command read while this side waits: true if it must yield (the
    // Sidecar), the command then held in pending_.
    bool noteIncomingCommand();
    // Read a new frame and keep it in pending_ if it's a command.
    bool takeCommand();
    // Wait (pumping) up to timeoutUs for the peer to ack our current frame.
    // While waiting, a new command from the peer is held in pending_.
    bool waitForAck(uint32_t timeoutUs);
    // For the host's polls: true when a response may be written now (our
    // current frame is acked or Idle), else pumps at most once, never
    // waiting, so the command stays pending for a later poll instead.
    bool readyToRespond();
    // Write tx_ as a new frame (mode, next frameSeq, current ackSeq). The
    // caller has waited for the ack.
    void writeNewFrame(SidecarMode mode);
    // The built-in commands both sides answer the same way: Nop, and reads
    // of the Test object. True if handled (tx_ holds the response).
    bool handleBuiltin(const SidecarHeader &header);
    // Answer the command held in pending_ (copied from the wire): the
    // response starts as a copy of the command with status 0, in work_;
    // handler fills it in (returns false to send no response). Then, once
    // the peer has acked our previous frame, it's written.
    template <typename Handler>
    bool respond(Handler &&handler);

    i2slink_t *link_ = nullptr;
    SidecarRole role_ = SidecarRole::Priority;
    bool loopback_ = false;
    bool primed_ = false;        // the first command since begin() was seen (and dropped)
    bool havePeer_ = false;      // a frame of ours version has been seen
    bool pendingValid_ = false;  // pending_ holds a command not yet answered
    uint16_t txSeq_ = 0;         // our current frameSeq
    uint16_t peerSeq_ = 0;       // the peer's last frameSeq (our ackSeq)
    uint16_t peerAck_ = 0;       // the peer's ackSeq (our frames it has seen)
    uint16_t pendingSeq_ = 0;    // pending_'s frameSeq
    uint32_t nextSequence_ = 0;  // command sequence numbers
    uint32_t preempted_ = 0, served_ = 0, versionMismatches_ = 0;
    i2slink_status_t lastStatus_ = I2SLINK_TIMEOUT;
    uint8_t rx_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t tx_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t pending_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t work_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t busy_ = 0;
};

// Host side: the priority peer.
class Sidecar : public SidecarPeer {
public:
    // Starts the link (clock role, I2S port and pins in config). Then call
    // probe() before the first command.
    esp_err_t begin(const i2slink_config_t &config);

    // Handshake: send Nop commands, each waited on for retryUs, until one is
    // answered or timeoutUs passes. True (and present()) if the Sidecar
    // answered. A command sent before a successful probe() may be dropped.
    bool probe(uint32_t timeoutUs, uint32_t retryUs = 20000);
    bool present() const { return present_; }

    // Send a command and wait up to timeoutUs for its response. These fill
    // in header.command and header.sequence and zero header.status;
    // header.flags, the body and data are the caller's. On Ok, response
    // holds the reply; otherwise response is untouched.
    SidecarResult nop(NopCommand &command, NopResponse &response, uint32_t timeoutUs);
    SidecarResult sio(SioCommand &command, SioResponse &response, uint32_t timeoutUs);
    SidecarResult cio(CioCommand &command, CioResponse &response, uint32_t timeoutUs);

    // Serve the Sidecar: ack its frames and answer at most one of its
    // commands (Nop, and reads of the host's objects). Cheap when nothing's
    // new (one i2slink_peek(), never a wait). Not called from the bmon loop
    // (main serves the Sidecar only in diagnostic mode, pollDiagnostic());
    // kept hot-safe (IRAM, memory only) all the same.
    bool poll();
    bool serviceOnce() { return poll(); }   // for sidecarFetch()

    // Diagnostic mode, from the Sidecar's reply to BootStatus (the host
    // sets it), left only by a reset. diagnosticFlags(): SidecarDiagnosticFlag
    // bits (Diagnostic always among them when any is set).
    bool diagnostic() const { return diagnosticFlags_ != 0; }
    uint8_t diagnosticFlags() const { return diagnosticFlags_; }
    void setDiagnosticFlags(uint8_t flags)
    {
        diagnosticFlags_ = flags ? flags | static_cast<uint8_t>(SidecarDiagnosticFlag::Diagnostic) : 0;
    }

    // Objects only served in diagnostic mode, because reading them reads
    // flash (for now Core, the core dump): poll() answers them Unavailable,
    // pollDiagnostic() serves them through reader(offset, buffer, length),
    // false = I/O error. Set it any time (main: at boot, from its boot
    // report); version is the token ReadVersion returns.
    using ObjectReader = bool (*)(uint32_t offset, void *buffer, uint32_t length);
    void setCoreObject(uint32_t size, uint32_t version, ObjectReader reader)
    {
        coreSize_ = size;
        coreVersion_ = version;
        coreReader_ = reader;
    }
    // poll() plus those objects. Cold: diagnostic mode only (bus detached,
    // interrupts on, flash reads allowed); never on the hot path.
    bool pollDiagnostic();

    // Byte-exact echo check: sends a Nop with a fresh pseudo-random body
    // through nop() and returns Ok only if the response matches it byte for
    // byte from the message header on (Mismatch if the header matched but
    // the rest differed). Works on a loopback link (the response is the
    // command itself, read back) and with a real Sidecar (which answers a
    // Nop with an exact copy). probe(), nop(), sio() and cio() also work in
    // loopback (each gets its own command back as the response).
    SidecarResult loopbackCheck(uint32_t timeoutUs);

    // Mount diskSpec (Sio syntax, components/disk_image/sio.h) in the
    // Sidecar's drive unitId (1 = D1:, as in the DCB's DUNIT), with a Mount
    // command answered by the Sidecar's own Sio. Waits up to timeoutUs (a
    // mount can inflate an .atr.gz or connect to SMB). True if the Sidecar
    // mounted it; otherwise false, with a message printed. Needs probe()
    // first. Not for the bus-time paths: it builds the command from a
    // std::string.
    bool mount(int unitId, const std::string &diskSpec, uint32_t timeoutUs = 10000000);

private:
    bool present_ = false;
    uint8_t diagnosticFlags_ = 0;   // from the Sidecar's BootStatus reply
    uint32_t coreSize_ = 0;
    uint32_t coreVersion_ = 0;
    ObjectReader coreReader_ = nullptr;
    // poll()'s answers (built-ins, Unavailable for Core),
    // shared with pollDiagnostic().
    bool handleHost(const SidecarHeader &header);
};

// Sidecar side: the subordinate peer. Answers the host's commands and may
// send its own.
//
// Call service() periodically from an outer loop. It never waits for a
// command: with nothing new it costs one i2slink_peek() (a couple of loads).
// When a command is there it reads it, runs its handler, writes the
// response and returns.
class SidecarService : public SidecarPeer {
public:
    virtual ~SidecarService() = default;
    esp_err_t begin(const i2slink_config_t &config);

    // Handle at most one command (one left pending by a preempted
    // transact() first). True if a command was answered.
    bool service();
    bool serviceOnce() { return service(); }   // for sidecarFetch()

protected:
    // Handlers, called from service(). The response starts as a copy of the
    // command (header, body and data) with header.status zeroed; the handler
    // edits it and may set header.status. The rest of the header (command,
    // sequence, flags) is restored afterwards, so it is always echoed.
    virtual void handleSio(const SioCommand &command, SioResponse &response) = 0;
    virtual void handleCio(const CioCommand &command, CioResponse &response) = 0;
    // Set response.header.status to a SidecarMountStatus.
    virtual void handleMount(const MountCommand &command, MountResponse &response) = 0;
    // Reads of the Sidecar's own objects (Test is built in): set
    // response.header.status to a SidecarReadStatus; the defaults answer
    // NoObject.
    virtual void handleReadVersion(const ReadVersionCommand &command, ReadVersionResponse &response);
    virtual void handleReadChunk(const ReadChunkCommand &command, ReadChunkResponse &response);
    virtual void handleNop() {}   // the echo is the answer; override to observe Nops
    // Main's boot report: decide now and set response.body.diagnostic (the
    // flags main must enter; it starts as 0, boot normally). Longer work
    // (fetching the core) belongs after service() returns.
    virtual void handleBootStatus(const BootStatusCommand &command, BootStatusResponse &response)
    {
        (void)command;
        (void)response;
    }

private:
    int64_t retryAfterUs_ = 0;   // link fault backoff (esp_timer time)
};

#include "sidecar_fetch.inc"
