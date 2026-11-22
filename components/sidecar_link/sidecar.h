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
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

#include "i2slink.h"
#include "atariStructs.h"   // AtariDCB, AtariIOCB
#include "sidecar_stream.h"

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
    SerialOut = 8,      // body: SidecarSerialOutBody, data: console text (main to Sidecar)
    EraseCore = 9,      // body: SidecarEraseCoreBody (Sidecar to main, diagnostic mode only)
    ConsoleIn = 10,     // body: SidecarConsoleInBody; response data: console input for main (main to Sidecar)
    // Main's files, from the Sidecar (diagnostic mode only: they touch
    // flash): one file open at a time, read through ReadChunk on the File
    // object, written with WriteChunk. See SidecarFileOpenBody.
    FileOpen = 11,      // body: SidecarFileOpenBody, data: the path (NUL-terminated)
    WriteChunk = 12,    // body: SidecarWriteChunkBody, data: the bytes
    FileClose = 13,     // body: SidecarFileCloseBody
    FileRemove = 14,    // body: SidecarFilePathBody, data: the path
    FileHash = 15,      // body: SidecarFileHashBody, data: the path
    Stream = 16,        // body: SidecarStreamBody, data: stream bytes (either direction)
    SystemControl = 17, // body: SidecarSystemControlBody (main to Sidecar)
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
    File = 4,       // the host: the file (or listing) FileOpen opened, read mode
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
// What a Sidecar's firmware can do beyond the original commands, in its
// BootStatus reply (capabilities). An older Sidecar echoes the command's 0
// there: main then never sends it commands it would leave unanswered (an
// unknown command gets no response, so the sender waits out its timeout).
enum class SidecarCapability : uint8_t {
    SerialOut = 0x01,   // it prints SerialOut text on its console
};
struct __attribute__((packed)) SidecarBootStatusBody {
    uint8_t resetReason;   // esp_reset_reason_t (ESP_RST_PANIC = 4, ...)
    uint8_t flags;         // SidecarBootFlag bits
    uint8_t diagnostic;    // command: 0; the reply: SidecarDiagnosticFlag bits main must enter
    uint8_t capabilities;  // command: 0; the reply: SidecarCapability bits (was reserved, 0)
    uint32_t coreSize;     // bytes, checksum included; 0 = none
    uint32_t coreVersion;  // the core dump's stored SHA-256 (no rehash): new dump, new token
    uint32_t appVersion;   // the running app's ELF SHA-256
    char git[24];          // the running app's GIT_VERSION, NUL-terminated
};

// SerialOut (main to Sidecar): length bytes of main's console output in
// data, as printf() wrote them (any split: a line may span frames). The
// Sidecar prints them on its own console, after answering (a blocked
// console must not hold up the response). level: the most severe A8LOG_*
// level in the chunk (components/a8log/include/a8log.h), for the
// Sidecar's use; 0 if unknown. The response's header.status: 0 printed (or
// queued to print), 1 dropped (no room), 2 BadRequest (length too big).
// EraseCore (Sidecar to main): erase main's core dump, once the Sidecar has a
// verified copy of it, so main's next boot report shows none: a core dump in
// main's flash is one not yet fetched, and the Sidecar keeps no other state.
// Only the one named: version is its token (BootStatus coreVersion, the
// Core object's version). Served in diagnostic mode only (it writes flash);
// outside it the answer is Unavailable. Erasing main's whole coredump
// partition takes seconds: allow for it. header.status: SidecarEraseStatus.
struct __attribute__((packed)) SidecarEraseCoreBody {
    uint32_t version;
};
enum class SidecarEraseStatus : uint8_t {
    Erased = 0,
    OtherCore = 1,     // main's core dump isn't the one named: left alone
    Unavailable = 2,   // no core dump, or not in diagnostic mode
    IoError = 3,
};

// Main's files (Sidecar to main, diagnostic mode only; outside it, or on an
// old main, no answer). Paths are VFS paths ("/spiffs/d1.atr.gz"), not
// checked: a bad one fails to open. One file is open at a time; FileOpen
// closes (and abandons) any other.
//   Read: FileOpen(Read) returns its size; ReadChunk(File, offset) reads it
//     (stateless, by offset, as for any object); FileClose.
//   Write: FileOpen(Write) opens <path>.part; WriteChunk(offset) appends:
//     offset must be the next byte, or the last chunk's again (a retry whose
//     answer was lost: acknowledged, not written twice); FileClose(commit 1)
//     renames it to <path> (replacing it), commit 0 deletes it. A failed copy
//     never leaves half a file.
//   List: FileOpen(List) on a directory makes its listing the file: text,
//     a line per entry, "<size> <name>\n" (size -1: a directory); read and
//     closed as a file.
// header.status: SidecarFileStatus; err: errno on Failed (0 otherwise).
enum class SidecarFileStatus : uint8_t {
    Ok = 0,
    Failed = 1,        // the file operation failed: err is its errno
    Unavailable = 2,   // no file open (WriteChunk, FileClose, ReadChunk File)
    BadRequest = 3,    // no NUL in the path, a bad mode, a write in read mode
    OutOfOrder = 4,    // WriteChunk at an offset that's neither the next nor the last again
};
enum class SidecarFileMode : uint8_t {
    Read = 0,
    Write = 1,
    List = 2,
};
struct __attribute__((packed)) SidecarFileOpenBody {
    uint8_t mode;        // SidecarFileMode
    uint8_t reserved[3];
    uint32_t size;       // response: the file's (or listing's) size; Write: 0
    int32_t err;         // response
};
struct __attribute__((packed)) SidecarWriteChunkBody {
    uint8_t reserved[2];
    uint16_t length;     // bytes of data
    uint32_t offset;
    int32_t err;         // response
};
struct __attribute__((packed)) SidecarFileCloseBody {
    uint8_t commit;      // Write: 1 rename <path>.part to <path>, 0 delete it
    uint8_t reserved[3];
    int32_t err;         // response
};
struct __attribute__((packed)) SidecarFilePathBody {
    int32_t err;         // response
};
// FileHash: the file's size and SHA-256 (whole file; main reads it all, at
// about 1 MB/s from SPIFFS: allow for it).
struct __attribute__((packed)) SidecarFileHashBody {
    uint32_t size;       // response
    int32_t err;         // response
    uint8_t sha256[32];  // response
};

// Stream (either direction): bytes of a channel's byte stream
// (SidecarStream, sidecar_stream.h). A generic pipe with minimal control:
// what a channel's bytes mean is up to the two apps.
enum class SidecarChannel : uint8_t {
    Telnet = 1,   // the Sidecar's telnet server: client keys to main, main's screen text back
};
static constexpr uint8_t kSidecarStreamChannels = 4;   // channel numbers 0..3
// flags: Open applies before this frame's data (a new session, positions
// from 0), Eof and Close after it, once all of it was taken.
enum class SidecarStreamFlag : uint8_t {
    Open = 0x01,
    Eof = 0x02,     // the sender has no more data (the receiver's read side ends)
    Close = 0x04,   // the session is over, both ways
};
// The receiver answers in header.status, and in the body: offset = how many
// of the session's bytes it now has, length = how many of this frame's it
// took (fewer when its buffer is full: the sender resends the rest).
enum class SidecarStreamStatus : uint8_t {
    Ok = 0,
    NoChannel = 1,   // no stream on that channel on this side
    Closed = 2,      // not in that session (never opened, closed, or restarted)
};
struct __attribute__((packed)) SidecarStreamBody {
    uint8_t channel;    // SidecarChannel
    uint8_t flags;      // SidecarStreamFlag bits
    uint16_t length;    // bytes in data (response: bytes taken)
    uint16_t session;   // the session id (the Open's)
    uint16_t reserved;  // 0
    uint32_t offset;    // the session position of data[0] (response: the receiver's position)
};

// SystemControl (main to Sidecar): the Sidecar's system flags and its bus
// sampler, in one message.
//
// Flags (SidecarSystemFlag): setFlags are set, then clearFlags cleared;
// every other flag keeps its value, so callers change only the bits they
// name. The response's flags are the result. A boot report (BootStatus)
// clears them all: main is starting again.
//
// The sampler (its burst captures of the bus pins against phi2,
// components/bus_profiler), by sampleAction (None leaves it alone): Start
// begins a sample period of the given category (a number main picks: what
// the bus is doing, e.g. 0 running, others for SIO, idle ...): each Start
// or category change ends the period before it, whose histograms the
// Sidecar prints labelled with its category. Stop ends the period and
// pauses sampling; Report prints the current period's histograms so far.
// Until main sends one, the Sidecar samples on its own (category 0). Every
// ended period also adds to its category's totals on the Sidecar
// (categories 0-15), kept across periods and runs until cleared: Totals
// prints them, ClearTotals clears them (category, or 255 every category).
// header.status: 0 done.
enum class SidecarSystemFlag : uint32_t {
    // Main's setup is done and core 1 serves the bus ("BOOT COMPLETE"): the
    // Sidecar releases the Atari's RESET, which it holds from its own start
    // and again whenever main (re)boots.
    BootComplete = 1u << 0,
};
enum class SidecarSampleAction : uint8_t {
    None = 0,
    Stop = 1,
    Start = 2,
    Report = 3,
    Totals = 4,
    ClearTotals = 5,
};
struct __attribute__((packed)) SidecarSystemControlBody {
    uint32_t setFlags;     // SidecarSystemFlag bits to set
    uint32_t clearFlags;   // ... then to clear
    uint8_t sampleAction;  // SidecarSampleAction
    uint8_t category;      // Start: the new period's category
    uint16_t reserved;     // 0
    uint32_t flags;        // response: the flags after the change
    uint32_t periods;      // response: periods the sampler has reported so far
};

struct __attribute__((packed)) SidecarSerialOutBody {
    uint8_t level;
    uint8_t reserved;   // 0
    uint16_t length;
};

// ConsoleIn (main to Sidecar): the console input the Sidecar holds for main
// (its console's "main <line>" lines, each with its newline), as if typed
// on main's own console: main parses it as it parses its USB console's
// input (handleSerialInput(): "exit 2", "key x", ...). The command's body is
// zero; the response's length says how many bytes of data follow (0: none
// waiting), and the Sidecar drops what it sent. No capability bit: every
// Sidecar app since it was added answers it (no older ones are deployed).
struct __attribute__((packed)) SidecarConsoleInBody {
    uint16_t length;
    uint16_t reserved;   // 0
};

using NopCommand = SidecarMessage<SidecarNopBody>;
using SioCommand = SidecarMessage<AtariDCB>;    // 279 data bytes: a 256-byte sector fits
using CioCommand = SidecarMessage<AtariIOCB>;   // 275 data bytes; chunk longer buffers
using MountCommand = SidecarMessage<SidecarMountBody>;
using ReadVersionCommand = SidecarMessage<SidecarReadVersionBody>;
using ReadChunkCommand = SidecarMessage<SidecarReadChunkBody>;
using BootStatusCommand = SidecarMessage<SidecarBootStatusBody>;
using SerialOutCommand = SidecarMessage<SidecarSerialOutBody>;   // 281 data bytes
using EraseCoreCommand = SidecarMessage<SidecarEraseCoreBody>;
using ConsoleInCommand = SidecarMessage<SidecarConsoleInBody>;
using FileOpenCommand = SidecarMessage<SidecarFileOpenBody>;
using WriteChunkCommand = SidecarMessage<SidecarWriteChunkBody>;
using FileCloseCommand = SidecarMessage<SidecarFileCloseBody>;
using FilePathCommand = SidecarMessage<SidecarFilePathBody>;   // FileRemove
using FileHashCommand = SidecarMessage<SidecarFileHashBody>;
using StreamCommand = SidecarMessage<SidecarStreamBody>;
using SystemControlCommand = SidecarMessage<SidecarSystemControlBody>;
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
using SerialOutResponse = SidecarMessage<SidecarSerialOutBody>;
using EraseCoreResponse = SidecarMessage<SidecarEraseCoreBody>;
using ConsoleInResponse = SidecarMessage<SidecarConsoleInBody>;
using FileOpenResponse = SidecarMessage<SidecarFileOpenBody>;
using WriteChunkResponse = SidecarMessage<SidecarWriteChunkBody>;
using FileCloseResponse = SidecarMessage<SidecarFileCloseBody>;
using FilePathResponse = SidecarMessage<SidecarFilePathBody>;
using FileHashResponse = SidecarMessage<SidecarFileHashBody>;
using StreamResponse = SidecarMessage<SidecarStreamBody>;
using SystemControlResponse = SidecarMessage<SidecarSystemControlBody>;

static_assert(sizeof(NopCommand) == I2SLINK_PAYLOAD_BYTES, "Nop message must fill the payload");
static_assert(sizeof(SioCommand) == I2SLINK_PAYLOAD_BYTES, "SIO message must fill the payload");
static_assert(sizeof(CioCommand) == I2SLINK_PAYLOAD_BYTES, "CIO message must fill the payload");
static_assert(sizeof(MountCommand) == I2SLINK_PAYLOAD_BYTES, "Mount message must fill the payload");
static_assert(sizeof(ReadChunkCommand) == I2SLINK_PAYLOAD_BYTES, "ReadChunk message must fill the payload");
static_assert(sizeof(SerialOutCommand) == I2SLINK_PAYLOAD_BYTES, "SerialOut message must fill the payload");
static_assert(sizeof(StreamCommand) == I2SLINK_PAYLOAD_BYTES, "Stream message must fill the payload");
static_assert(sizeof(SystemControlCommand) == I2SLINK_PAYLOAD_BYTES, "SystemControl message must fill the payload");
static_assert(sizeof(ConsoleInCommand) == I2SLINK_PAYLOAD_BYTES, "ConsoleIn message must fill the payload");
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
    // EraseCore (Sidecar to main): *status is main's SidecarEraseStatus when
    // the result is Ok.
    SidecarResult eraseCore(uint32_t version, SidecarEraseStatus *status, uint32_t timeoutUs);

    // Byte streams (Stream commands, sidecar_stream.h): attach this side's
    // end of a channel; the peer's Stream commands for it go to
    // stream.handleCommand(), others are answered NoChannel. Attach before
    // the link is used; the stream must outlive the peer.
    void attachStream(SidecarStream &stream);

    i2slink_status_t lastLinkStatus() const { return lastStatus_; }
    i2slink_t *link() const { return link_; }
    bool loopback() const { return loopback_; }

    // Counters, for logs.
    uint32_t preempted() const { return preempted_; }
    uint32_t served() const { return served_; }
    // Every response this side wrote: its command's NEW read to the
    // response's write, in power-of-two us buckets (bucket b: under
    // 2^(b+3) us; the last, the rest). The Sidecar's 20 s line.
    struct ResponseTimes {
        uint32_t hist[14];
    };
    ResponseTimes responseTimes() const { return responseTimes_; }
    // Measuring: this side's successful transact()s: from the call to its
    // command written (the ack wait and the write), and from there to the
    // response's NEW read (cycles, summed).
    struct TransactTiming {
        uint32_t count;
        uint64_t toWritten, toResponse;
    };
    TransactTiming transactTiming() const { return transactTiming_; }
    void resetTransactTiming() { transactTiming_ = {}; }
    uint32_t versionMismatches() const { return versionMismatches_; }
    // takeCommand()'s true hints: polls that went on to read a frame.
    uint32_t hintReads() const { return hintReads_; }

    // Inside one of this side's calls (transact, poll, service): csim's
    // simulated link runs a side's poll only when it isn't.
    bool busy() const { return busy_ != 0; }

    // Scratch messages for the cold one-shot calls (probe, mount, fetches,
    // the boot report), so a command and its response aren't on the
    // caller's stack: at the largest frames (i2slink.h) a pair is ~3.4 KB,
    // more than main's task stack has to spare. One pair: those calls never
    // nest (transact() is busy-guarded; LOG frames use serialOut()'s own).
    // scratchCommand() zeroes it, as "= {}" did.
    template <typename Message> Message &scratchCommand() {
        static_assert(sizeof(Message) == I2SLINK_PAYLOAD_BYTES, "a link message");
        return *new (scratchCommand_) Message{};
    }
    template <typename Message> Message &scratchResponse() {
        static_assert(sizeof(Message) == I2SLINK_PAYLOAD_BYTES, "a link message");
        return *new (scratchResponse_) Message;
    }

protected:
    struct BusyScope {
        uint8_t &count;
        explicit BusyScope(uint8_t &c) : count(c) { ++count; }
        ~BusyScope() { --count; }
    };

    // Read at most one new frame, waiting up to waitUs. Updates the peer's
    // ack and acks the frame. Returns true if rx_ holds a new frame (a new
    // frameSeq) of our version.
    // deferCommandAck: a new Command frame isn't acked here (ackOwed_):
    // its response, written soon after, carries the ack (takeCommand(true)).
    bool pump(uint32_t waitUs, bool deferCommandAck = false);
    // The ack pump() deferred, as an ack-only rewrite, if still owed.
    void flushOwedAck();
    // A command read while this side waits: true if it must yield (the
    // Sidecar), the command then held in pending_.
    bool noteIncomingCommand();
    // Read a new frame and keep it in pending_ if it's a command.
    // deferAck: for a caller that answers the command at once (the Sidecar's
    // service()), its ack goes in the response (pump()).
    bool takeCommand(bool deferAck = false);
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
    // A Stream command in pending_, answered in work_ (both sides).
    void handleStream();
    // Answer the command held in pending_ (copied from the wire): the
    // response starts as a copy of the command with status 0, in work_;
    // handler fills it in (returns false to send no response). Then, once
    // the peer has acked our previous frame, it's written.
    template <typename Handler>
    bool respond(Handler &&handler);

    i2slink_t *link_ = nullptr;
    SidecarRole role_ = SidecarRole::Priority;
    bool loopback_ = false;
    bool primed_ = false;        // past the command on the wire at begin(), if any (pump(), respond())
    bool havePeer_ = false;      // a frame of ours version has been seen
    bool pendingValid_ = false;  // pending_ holds a command not yet answered
    uint16_t txSeq_ = 0;         // our current frameSeq
    uint16_t peerSeq_ = 0;       // the peer's last frameSeq (our ackSeq)
    uint16_t peerAck_ = 0;       // the peer's ackSeq (our frames it has seen)
    uint16_t pendingSeq_ = 0;    // pending_'s frameSeq
    uint32_t nextSequence_ = 0;  // command sequence numbers
    uint32_t preempted_ = 0, served_ = 0, versionMismatches_ = 0, hintReads_ = 0;
    uint32_t newAt_ = 0;   // ticksNow() at the last NEW frame (pump())
    bool ackOwed_ = false;  // pump() deferred the ack of peerSeq_ (deferCommandAck)
    ResponseTimes responseTimes_ = {};
    TransactTiming transactTiming_ = {};
    i2slink_status_t lastStatus_ = I2SLINK_TIMEOUT;
    // Four link payloads. Static (main's sidecarHost is in .data): a bigger
    // frame (i2slink.h) only pushes main's .bss, and core 1's hot data at
    // its front, further from core 0's interrupt stack; a smaller .data
    // than today's could slide it into that bank (main/mmu.h; the post-
    // link check, tools/verify_core1_hot_data.py, fails the build then).
    uint8_t rx_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t tx_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t pending_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t work_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t scratchCommand_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t scratchResponse_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t busy_ = 0;
    SidecarStream *streams_[kSidecarStreamChannels] = {};
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
    // Send len bytes (at most SerialOutCommand::kDataBytes) of console text
    // for the Sidecar to print (SerialOut). Ok means it took them
    // (response.header.status 0). Only to a Sidecar whose BootStatus reply
    // had SidecarCapability::SerialOut (capabilities()). Hot-safe (IRAM,
    // memory only); uses a message buffer of its own.
    SidecarResult serialOut(const void *data, uint16_t len, uint8_t level, uint32_t timeoutUs);
    // SystemControl: set, then clear, flag bits (SidecarSystemFlag; others
    // unchanged), and a sampler action (None: none). *flags (if given): the
    // Sidecar's flags after it. Hot-safe (IRAM, memory only); the scratch
    // buffers (scratchCommand()), so not while a cold helper is mid-call.
    SidecarResult systemControl(uint32_t setFlags, uint32_t clearFlags, SidecarSampleAction action,
                                uint8_t category, uint32_t timeoutUs, uint32_t *flags = nullptr);
    // Ask for the console input the Sidecar holds for main (ConsoleIn): on
    // Ok, response.body.length bytes of it in response.data. Cold (the
    // caller's messages; called with the bus detached).
    SidecarResult consoleIn(ConsoleInCommand &command, ConsoleInResponse &response, uint32_t timeoutUs);
    // The Sidecar's SidecarCapability bits, from its BootStatus reply (set
    // by the host); 0 before that or from an older Sidecar.
    uint8_t capabilities() const { return capabilities_; }
    void setCapabilities(uint8_t caps) { capabilities_ = caps; }

    // Serve the Sidecar: ack its frames and answer at most one of its
    // commands (Nop, and reads of the host's objects). Cheap when nothing's
    // new (one i2slink_hint()). Not called from the bmon loop
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
    // EraseCore, in pollDiagnostic(): eraser() erases the core dump (true:
    // done); the Core object is gone after it.
    using ObjectEraser = bool (*)();
    void setCoreEraser(ObjectEraser eraser) { coreEraser_ = eraser; }
    // Anything else pollDiagnostic() serves (main's files: FileOpen ...):
    // handler(command, message) gets the command in message, a copy that
    // becomes the response (it sets the header's status, the body and the
    // data), and returns true; false: not its command (the default
    // answers follow). ReadVersion/ReadChunk of objects other than Core
    // come here too.
    using DiagnosticHandler = bool (*)(SidecarCommand command, void *message);
    void setDiagnosticHandler(DiagnosticHandler handler) { diagnosticHandler_ = handler; }
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
    uint8_t capabilities_ = 0;      // likewise
    SerialOutCommand serialOutCommand_;
    SerialOutResponse serialOutResponse_;
    uint32_t coreSize_ = 0;
    uint32_t coreVersion_ = 0;
    ObjectReader coreReader_ = nullptr;
    ObjectEraser coreEraser_ = nullptr;
    DiagnosticHandler diagnosticHandler_ = nullptr;
    // poll()'s answers (built-ins, Unavailable for Core),
    // shared with pollDiagnostic().
    bool handleHost(const SidecarHeader &header);
};

// Sidecar side: the subordinate peer. Answers the host's commands and may
// send its own.
//
// Call service() periodically from an outer loop. It never waits for a
// command: with nothing new it costs one i2slink_hint() (a couple of loads).
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

    // The host's file commands (FileOpen, ReadChunk of the File object ...:
    // a SidecarFileServer's serve()) go to handler first; false: not its.
    using FileHandler = bool (*)(SidecarCommand command, void *message);
    void setFileHandler(FileHandler handler) { fileHandler_ = handler; }

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
    // Main's console text: copy it out and print it after service()
    // returns, not here (the response waits on this). Set
    // response.header.status (SidecarSerialOutBody). The default drops it
    // (status 1); a Sidecar that overrides this advertises
    // SidecarCapability::SerialOut in its BootStatus reply.
    virtual void handleSerialOut(const SerialOutCommand &command, SerialOutResponse &response)
    {
        (void)command;
        response.header.status = 1;
    }
    // Main asks for its console input: put up to ConsoleInResponse::kDataBytes
    // bytes in response.data and their count in response.body.length. The
    // default has none.
    virtual void handleConsoleIn(const ConsoleInCommand &command, ConsoleInResponse &response)
    {
        (void)command;
        response.body.length = 0;
    }
    // Main's SystemControl: act on command.body, response.body.flags the
    // result; response.header.status 0 done (the default: 1, not handled).
    virtual void handleSystemControl(const SystemControlCommand &command, SystemControlResponse &response)
    {
        (void)command;
        response.header.status = 1;
    }
    // Main's boot report: decide now and set response.body.diagnostic (the
    // flags main must enter; it starts as 0, boot normally). Longer work
    // (fetching the core) belongs after service() returns.
    virtual void handleBootStatus(const BootStatusCommand &command, BootStatusResponse &response)
    {
        (void)command;
        (void)response;
    }

    FileHandler fileHandler_ = nullptr;
private:
    int64_t retryAfterUs_ = 0;   // link fault backoff (esp_timer time)
};

#include "sidecar_fetch.inc"
