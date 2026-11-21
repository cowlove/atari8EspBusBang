#pragma once

// SidecarStream: one end of a byte stream over the link (the Stream
// command, sidecar.h), for a channel (SidecarChannel). Either side opens,
// sends and closes; the link carries the bytes in Stream commands, each
// answered with how far the receiver got, so a lost or retried frame never
// duplicates or drops data.
//
// Sessions: open() starts a new session (a 16-bit id, both directions'
// positions back to 0); the peer adopts it on the Open frame. Data in a
// session the receiver isn't in is refused (status Closed), so a side that
// restarted makes the other see its stream close.
//
// Threads: one link task (the one calling the peer's transact(), poll() or
// service()) runs the handler and pump(); the app may run in another task:
// write(), read(), open(), close() and the event counters are single-
// producer/single-consumer safe against the link task. On main everything
// runs in core 0's task, and the handler (handleCommand(), from poll()) is
// hot-safe: IRAM, memory only.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_attr.h"

class SidecarPeer;
struct SidecarStreamBody;

class SidecarStream {
public:
    // rxBuffer/txBuffer: capacities a power of two. channel: SidecarChannel.
    void init(uint8_t channel, uint8_t *rxBuffer, uint32_t rxBytes, uint8_t *txBuffer, uint32_t txBytes);
    uint8_t channel() const { return channel_; }

    // App side.
    // Start a new session (sent by the next pump()); queued data goes after
    // the Open.
    void open() { requestOpen_.store(true); }
    // End the session once the queued data is sent.
    void close() { requestClose_.store(true); }
    // Queue up to len bytes to send; returns how many fit.
    uint32_t write(const uint8_t *data, uint32_t len);
    uint32_t txFree() const;
    uint32_t txQueued() const;
    // Take up to max received bytes; returns how many.
    uint32_t read(uint8_t *data, uint32_t max);
    uint32_t rxAvailable() const;
    // In a session (opened by either side, not yet closed).
    bool isOpen() const { return open_.load(); }
    // Event counters: the app keeps the last values it saw. opens(): each
    // session start (ours or the peer's); closes(): each end (either side's
    // Close, or the peer refusing our data: it isn't in our session).
    uint32_t opens() const { return opens_.load(); }
    uint32_t closes() const { return closes_.load(); }

    // Link task side.
    // Send what's pending (an Open, queued data, a Close) in one Stream
    // command. True if anything was sent and answered; false if there was
    // nothing to send or the link didn't answer (pump again later).
    bool pump(SidecarPeer &peer, uint32_t timeoutUs);
    bool pending() const { return requestOpen_.load() || requestClose_.load() || txQueued() != 0; }
    // The Stream command handler (the receiving side). Fills the response's
    // body and returns its header.status (SidecarStreamStatus).
    uint8_t handleCommand(const SidecarStreamBody &command, const uint8_t *data, SidecarStreamBody &response);

    // Counters, for logs.
    uint32_t rxBytes() const { return rxTotal_; }
    uint32_t txBytes() const { return txTotal_; }
    uint32_t rxDropped() const { return rxDropped_; }   // refused: not in the session, or no room

private:
    void startSession(uint16_t session);
    void endSession();
    __attribute__((always_inline)) void discardTx() { txTail_.store(txHead_.load()); }

    uint8_t channel_ = 0;
    uint8_t *rx_ = nullptr;
    uint8_t *tx_ = nullptr;
    uint32_t rxMask_ = 0, txMask_ = 0;
    // Free-running ring indices: head written by the producer, tail by the
    // consumer.
    std::atomic<uint32_t> rxHead_{0}, rxTail_{0}, txHead_{0}, txTail_{0};
    std::atomic<bool> requestOpen_{false}, requestClose_{false}, open_{false};
    std::atomic<uint32_t> opens_{0}, closes_{0};
    uint16_t session_ = 0;     // link task only
    uint16_t nextSession_ = 0;
    uint32_t rxPos_ = 0;       // bytes of this session received
    uint32_t txPos_ = 0;       // bytes of this session the peer has taken
    uint32_t rxTotal_ = 0, txTotal_ = 0, rxDropped_ = 0;
};
