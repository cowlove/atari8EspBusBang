#pragma once

// Sidecar: an optional second ESP32 that works as a general coprocessor for
// the host (this board): disk/SIO and CIO IO for now; later perhaps video,
// sound, LEDs and bus monitoring the host has no pins or time for.
//
// The host and the Sidecar talk over i2slink (the hands-free I2S DMA link,
// i2slink.h), with one command/response protocol:
//   - Sidecar (host side) sends a command and waits for its response;
//   - SidecarService (Sidecar side) answers commands, from an outer loop.
//
// What the protocol relies on from i2slink:
//   - each direction carries the latest value, not a queue;
//   - a value stays on the wire until its writer writes again, so one write
//     of a command is enough: the hardware resends it every wire cycle;
//   - read() returns each version at most once (NEW is always new).
//
// So a transaction is: write the command once, then read responses until
// one echoes this command's command byte and sequence number. Responses to
// other commands (e.g. left on the wire from before a restart) are ignored.
//
// Restarts. A SidecarService that restarts finds the host's last command
// still on the wire, reading as NEW; it may already have run it, so it drops
// the first command it sees after begin(). The host's probe() handshake
// (Nop commands, resent until one is answered) absorbs that drop at startup,
// and tells the host whether a Sidecar is there at all.
//
// Single-threaded: all calls on one Sidecar (or SidecarService) from one task
// (i2slink's hint/read/write all run in the caller's context).

#include <cstddef>
#include <cstdint>

#include "i2slink.h"
#include "atariStructs.h"   // AtariDCB, AtariIOCB

enum class SidecarCommand : uint8_t {
    Nop = 0,   // body: none; answered with an echo (probe(), presence)
    Sio = 1,   // body: AtariDCB, then optional raw data for the DCB
    Cio = 2,   // body: AtariIOCB, then optional raw data for the IOCB
};

struct __attribute__((packed)) SidecarHeader {
    uint8_t command;     // SidecarCommand
    uint32_t sequence;   // chosen by the host; the response echoes it
    uint8_t flags;       // reserved for future expansion; 0 for now
    uint8_t status;      // commands: 0. Responses: set by the handler (0 if it
                         // leaves it alone); its meaning is per command
};

// One whole link payload: header, the command-specific struct, then raw
// data filling the rest. Sends always carry the full payload size.
template <typename Body>
struct __attribute__((packed)) SidecarMessage {
    static constexpr size_t kDataBytes =
        I2SLINK_PAYLOAD_BYTES - sizeof(SidecarHeader) - sizeof(Body);

    SidecarHeader header;
    Body body;
    uint8_t data[kDataBytes];   // only the bytes the DCB/IOCB calls for are meaningful
};

struct __attribute__((packed)) SidecarNopBody {
    uint8_t reserved;   // 0
};

using NopCommand = SidecarMessage<SidecarNopBody>;
using SioCommand = SidecarMessage<AtariDCB>;    // 285 data bytes: a 256-byte sector fits
using CioCommand = SidecarMessage<AtariIOCB>;   // 281 data bytes; chunk longer buffers
// For now the responses mirror the commands exactly.
using NopResponse = SidecarMessage<SidecarNopBody>;
using SioResponse = SidecarMessage<AtariDCB>;
using CioResponse = SidecarMessage<AtariIOCB>;

static_assert(sizeof(NopCommand) == I2SLINK_PAYLOAD_BYTES, "Nop message must fill the payload");
static_assert(sizeof(SioCommand) == I2SLINK_PAYLOAD_BYTES, "SIO message must fill the payload");
static_assert(sizeof(CioCommand) == I2SLINK_PAYLOAD_BYTES, "CIO message must fill the payload");

enum class SidecarResult {
    Ok,          // response received; it echoes the command's header
    Timeout,     // link healthy, but no matching response in time
    LinkFault,   // the link itself failed (see lastLinkStatus())
};

// Host side.
class Sidecar {
public:
    // Starts the link (clock role, I2S port and pins in config). Then call
    // probe() before the first command.
    esp_err_t begin(const i2slink_config_t &config);
    void end();

    // Handshake: send Nop commands, each waited on for retryUs, until one is
    // answered or timeoutUs passes. True (and present()) if the Sidecar
    // answered. A command sent before a successful probe() may be dropped.
    bool probe(uint32_t timeoutUs, uint32_t retryUs = 20000);
    bool present() const { return present_; }

    // Send a command and wait up to timeoutUs for its response. These fill
    // in header.command and header.sequence and zero header.status;
    // header.flags, the body and data are the caller's. On Ok, response holds the reply; otherwise
    // response is untouched.
    SidecarResult nop(NopCommand &command, NopResponse &response, uint32_t timeoutUs);
    SidecarResult sio(SioCommand &command, SioResponse &response, uint32_t timeoutUs);
    SidecarResult cio(CioCommand &command, CioResponse &response, uint32_t timeoutUs);

    // The i2slink status behind the last LinkFault or Timeout.
    i2slink_status_t lastLinkStatus() const { return lastStatus_; }
    i2slink_t *link() const { return link_; }

private:
    SidecarResult transact(SidecarCommand command, SidecarHeader &header,
                           const void *commandBytes, void *responseBytes, uint32_t timeoutUs);

    i2slink_t *link_ = nullptr;
    bool present_ = false;
    uint32_t nextSequence_ = 0;
    i2slink_status_t lastStatus_ = I2SLINK_TIMEOUT;
    uint8_t rx_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
};

// Sidecar side: answers commands from the host.
//
// Call service() periodically from an outer loop. It never waits for a
// command: with nothing new it costs one i2slink_hint() (a couple of loads).
// When a command may be there it reads it (i2slink_read(..., 0): up to about
// one wire cycle, 312 us, for the beam window), runs its handler, writes the
// response and returns.
//
// Each command is answered once, with its header echoed. The first command
// seen after begin() is dropped (see "Restarts" above). Nop is answered
// here; the Sidecar app subclasses this and implements the other handlers.
class SidecarService {
public:
    virtual ~SidecarService() = default;
    esp_err_t begin(const i2slink_config_t &config);
    void end();

    // Handle at most one command. True if a command was handled (a response
    // written).
    bool service();

    i2slink_status_t lastLinkStatus() const { return lastStatus_; }
    i2slink_t *link() const { return link_; }

protected:
    // Handlers, called from service(). The response starts as a copy of the
    // command (header, body and data) with header.status zeroed; the handler
    // edits it and may set header.status. The rest of the header (command,
    // sequence, flags) is restored afterwards, so it is always echoed.
    virtual void handleSio(const SioCommand &command, SioResponse &response) = 0;
    virtual void handleCio(const CioCommand &command, CioResponse &response) = 0;

private:
    i2slink_t *link_ = nullptr;
    bool primed_ = false;              // the first command since begin() was seen
    int64_t retryAfterUs_ = 0;         // link fault backoff (esp_timer time)
    i2slink_status_t lastStatus_ = I2SLINK_TIMEOUT;
    uint8_t rx_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
    uint8_t tx_[I2SLINK_PAYLOAD_BYTES] __attribute__((aligned(4)));
};
