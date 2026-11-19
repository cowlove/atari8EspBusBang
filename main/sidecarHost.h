#pragma once

// The host's layer over the Sidecar coprocessor (A8BB_SIDECAR_ENABLE).
// components/sidecar_link/sidecar.h is only the link protocol, shared with
// the Sidecar app; what the host does with it lives here: starting the link,
// which drives the Sidecar has mounted, and spotting the PBI requests that
// belong to the Sidecar. Host types (PbiIocb, the Atari's DCB) stay on this
// side.

#include "sdkconfig.h"

#if CONFIG_A8BB_SIDECAR_ENABLE
#include <stdarg.h>
#include <stdint.h>
#include <string>

#include "core0_hotspots.h"
#include "sidecar.h"

struct PbiIocb;
struct SidecarSioRecord;

class SidecarHost {
public:
    // Start the link (the host drives the clock; the Sidecar follows), probe
    // for the Sidecar and log the result. Call once from setup().
    void begin();

    // Mount diskSpec in the Sidecar's drive unitId (1 = D1:) and track the
    // result: the unit counts as the Sidecar's only if the Sidecar reports it
    // mounted (not for an empty diskSpec, which unmounts, or any failure).
    bool mount(int unitId, const std::string &diskSpec);

    // Called first in handlePbiRequest(): true if the request is the
    // Sidecar's, i.e. an SIO request (cmd 7) for a disk (DDEVIC & 0x7f ==
    // $31) whose DUNIT the Sidecar has mounted. Core 0 with interrupts
    // masked and core 1 running the bus: IRAM, no flash, no peripherals.
    CORE0_HOTSPOTS_ATTR bool checkPbiRequestHook(PbiIocb *pbiRequest);

    // Handle a request checkPbiRequestHook() claimed, in place of
    // handlePbiRequest2(): returns its RES_FLAG_* result. For now only SIO:
    // the DCB handling mirrors handlePbiRequest2()'s (the same commands, the
    // same MMU check and copy-in/copy-out through pbiROM[0x400]), with the
    // IO itself done by the Sidecar (Sidecar::sio()). Same constraints as
    // checkPbiRequestHook().
    CORE0_HOTSPOTS_ATTR int handlePbiRequest(PbiIocb *pbiRequest);

    // Bit n set: the Sidecar has DUNIT n mounted.
    uint32_t mountedUnits() const { return mountedUnits_; }

    // SIO requests the Sidecar answered, and ones lost to the link
    // (timeout or link fault: the Atari gets DSTATS 138).
    uint32_t sioRequests() const { return sioRequests_; }
    uint32_t sioFailures() const { return sioFailures_; }
    // CPU cycles the answered SIO round trips took (Sidecar::sio()), in all
    // and the longest: the link's latency, per request.
    uint64_t sioTicks() const { return sioTicks_; }
    uint32_t sioTicksMax() const { return sioTicksMax_; }

    Sidecar &sidecar() { return sidecar_; }

    // Link v2: ack and serve the Sidecar's commands (Nop, reads of the
    // host's objects). core0_utilityTasks() calls this every pass; a Sidecar
    // command waits for it. Hot-safe (IRAM, memory only).
    CORE0_HOTSPOTS_ATTR bool poll() { return sidecar_.poll(); }
    // The bmon loop's periodic poll (core0_utilityTasks(), exp-txalign):
    // polls, commands served, and the slowest poll in CCOUNT ticks.
    uint32_t polls = 0, pollsServed = 0, pollTicksMax = 0;
    // Diagnostic mode (from the Sidecar's reply to the boot report; only a
    // reset leaves it): setup() never attaches the bus and only polls the link.
    CORE0_HOTSPOTS_ATTR bool diagnostic() const { return sidecar_.diagnostic(); }
    uint8_t diagnosticFlags() const { return sidecar_.diagnosticFlags(); }
    // Diagnostic mode's poll: poll() plus the Core object (reads flash).
    bool pollDiagnostic() { return sidecar_.pollDiagnostic(); }

    // Fetch an object from the Sidecar (sidecarFetch()) and print how it
    // went: size, CRC32 against the version token, time; Test is also
    // checked byte by byte. Cold paths only (a serial command, csim).
    void fetchReport(SidecarObject object);
    // LINK_TEST: n single round trips (Nop, or SIO status D1:), each after a
    // random 0-500 us gap (a request at a random point of the wire cycle,
    // as the 6502's are), timed: min/mean/max and a 50 us histogram.
    void roundTripReport(bool sio, int n);
    // LINK_TEST: n Nops, each written just ahead of main's own TX slot
    // (found from its TX DMA's register), timed from that slot.
    void slotRoundTripReport(int n, unsigned descsBefore, int delayUs);
    // Diagnostic mode's OTA: fetch the Sidecar's Firmware object into the
    // next app slot and restart into it; returns only on failure.
    bool installFirmware();
    CORE0_HOTSPOTS_ATTR bool probe(uint32_t tmo) { return sidecar_.probe(tmo); } 

    // LOG()/LOGHOT() text for the Sidecar's console (main/a8log.cpp,
    // CONFIG_A8BB_LOG_VIA_SIDECAR), sent synchronously, line-buffered: a
    // SerialOut frame at each newline, or when the frame is full. Core 0's
    // task only.
    //   log(): LOG()'s; formats with vsnprintf (cold: floats and all).
    //   logHot(): LOGHOT()'s; formats with nanoprintf (IRAM), hot-safe.
    // Both return the formatted length. Not re-entrant (logPut()'s
    // inProgress, and no sending inside another link call): a log line
    // from in there (sidecarHost and below) is not sent (logReentered()).
    // A frame the link doesn't take is dropped; three unanswered in a row
    // turn the route off (a8logSidecarStop()).
    int log(int level, const char *format, va_list args);
    CORE0_HOTSPOTS_ATTR int logHot(int level, const char *format, va_list args);
    // Send a partial line still in the buffer.
    CORE0_HOTSPOTS_ATTR void logFlush();
    uint32_t logFrames() const { return logFrames_; }
    uint32_t logFailures() const { return logFailuresTotal_; }
    uint32_t logReentered() const { return logReentered_; }
    // CPU cycles the sent frames took (round trips), in all and the longest.
    uint64_t logTicks() const { return logTicks_; }
    uint32_t logTicksMax() const { return logTicksMax_; }
private:
    CORE0_HOTSPOTS_ATTR void logPut(const char *text, size_t len, int level);
    CORE0_HOTSPOTS_ATTR void logSendLine();
    // The once-per-boot report to the Sidecar (BootStatus); from begin().
    void sendBootStatus();

    // handlePbiRequest()'s SIO handling. With the struct logs on, log is the
    // request's record and this fills in its buffer fields (len, path,
    // buffer, data) for handlePbiRequest() to log; otherwise it is null.
    CORE0_HOTSPOTS_ATTR int handleSio(PbiIocb *pbiRequest, SidecarSioRecord *log);

    Sidecar sidecar_;
    uint32_t mountedUnits_ = 0;
    uint32_t sioRequests_ = 0;
    uint32_t sioFailures_ = 0;
    uint64_t sioTicks_ = 0;
    uint32_t sioTicksMax_ = 0;
    uint32_t logFrames_ = 0;          // SerialOut frames the Sidecar took
    uint64_t logTicks_ = 0;
    uint32_t logTicksMax_ = 0;
    uint32_t logFailuresTotal_ = 0;
    uint32_t logReentered_ = 0;
    uint8_t logFailuresInRow_ = 0;
    bool logInProgress_ = false;
    uint8_t logLevel_ = 0;            // the most severe level in logLine_ (0: none)
    uint16_t logLen_ = 0;
    char logLine_[SerialOutCommand::kDataBytes];   // the line being built
};

extern DRAM_ATTR SidecarHost sidecarHost;
#endif
