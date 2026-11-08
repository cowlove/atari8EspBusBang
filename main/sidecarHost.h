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

private:
    // handlePbiRequest()'s SIO handling. Fills in rec's buffer fields
    // (len, path, buffer, data); handlePbiRequest() logs it.
    CORE0_HOTSPOTS_ATTR int handleSio(PbiIocb *pbiRequest, SidecarSioRecord &rec);

    Sidecar sidecar_;
    uint32_t mountedUnits_ = 0;
    uint32_t sioRequests_ = 0;
    uint32_t sioFailures_ = 0;
};

extern DRAM_ATTR SidecarHost sidecarHost;
#endif
