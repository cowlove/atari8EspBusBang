#pragma once

// The host's layer over the Sidecar coprocessor (A8BB_SIDECAR_ENABLE).
// components/sidecar_link/sidecar.h is only the link protocol, shared with
// the Sidecar app; what the host does with it lives here: starting the link,
// which drives the Sidecar has mounted, and spotting the PBI requests that
// belong to the Sidecar. Host types (PbiIocb, the Atari's DCB) stay on this
// side.

#include "sdkconfig.h"

#if CONFIG_A8BB_SIDECAR_ENABLE
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

    Sidecar &sidecar() { return sidecar_; }

    // Link v2: ack and serve the Sidecar's commands (Nop, reads of the
    // host's objects). core0_utilityTasks() calls this every pass; a Sidecar
    // command waits for it. Hot-safe (IRAM, memory only).
    CORE0_HOTSPOTS_ATTR bool poll() { return sidecar_.poll(); }
    // Diagnostic mode (the Sidecar's SetDiagnostic; only a reset leaves it):
    // core0_utilityTasks() stops there and only polls the link.
    CORE0_HOTSPOTS_ATTR bool diagnostic() const { return sidecar_.diagnostic(); }

    // Fetch an object from the Sidecar (sidecarFetch()) and print how it
    // went: size, CRC32 against the version token, time; Test is also
    // checked byte by byte. Cold paths only (a serial command, csim).
    void fetchReport(SidecarObject object);
    CORE0_HOTSPOTS_ATTR bool probe(uint32_t tmo) { return sidecar_.probe(tmo); } 
private:
    // handlePbiRequest()'s SIO handling. With the struct logs on, log is the
    // request's record and this fills in its buffer fields (len, path,
    // buffer, data) for handlePbiRequest() to log; otherwise it is null.
    CORE0_HOTSPOTS_ATTR int handleSio(PbiIocb *pbiRequest, SidecarSioRecord *log);

    Sidecar sidecar_;
    uint32_t mountedUnits_ = 0;
    uint32_t sioRequests_ = 0;
    uint32_t sioFailures_ = 0;
};

extern DRAM_ATTR SidecarHost sidecarHost;
#endif
