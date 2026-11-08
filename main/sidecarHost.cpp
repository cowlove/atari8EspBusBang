#include "sidecarHost.h"

#if CONFIG_A8BB_SIDECAR_ENABLE
#include "pinDefs.h"

#include <stdio.h>
#include <string.h>

#include "asmDefs.h"   // REQ_FLAG_*, RES_FLAG_*
#include "main.h"      // atariMem (the DCB), ioCount, lastIoSec, elapsedSec
#include "mmu.h"       // mmuCheckRangeMapped(), mmuState, pbiROM
#include "pbi.h"       // PbiIocb
#include "log.h"       // structLogs: fixed rings, safe with core 1 hot

DRAM_ATTR SidecarHost sidecarHost;

namespace {
constexpr uint8_t kPbiCmdSio = 7;       // PbiIocb::cmd for SIO (see handlePbiRequest2())
constexpr uint8_t kDiskDevice = 0x31;   // DDEVIC for D1:...D8: (low 7 bits)

// How long one SIO command may take on the Sidecar, link faults included.
// The 6502 waits in the stack program meanwhile, with no timeout of its
// own. 6 s covers an in-place SPIFFS write that triggers garbage
// collection (seen at 2.6 s on the Sidecar); it stays far below the 17 s
// CCOUNT wrap (see sidecar.cpp), but core 0 is busy for the whole wait.
constexpr uint32_t kSioTimeoutUs = 6000000;

constexpr uint8_t kAtariTimeout = 0x8a;   // DSTATS 138: device timeout
constexpr uint8_t kAtariNak = 0x8b;       // DSTATS 139: device NAK

// The SIO commands handlePbiRequest2() knows, by which way data moves.
// Writes: the Atari sends DBYT bytes.
inline bool sioSendsData(uint8_t command)
{
    return command == 0x50 || command == 0xd0 || command == 0x57 || command == 0x4f;
}
// Reads: the drive returns data (status: 4 bytes; the rest: DBYT).
inline bool sioReturnsData(uint8_t command)
{
    return command == 0x52 || command == 0xd2 || command == 0x53 || command == 0x4e ||
           command == 0x3f;
}
}  // namespace

CORE0_HOTSPOTS_ATTR int SidecarHost::handlePbiRequest(PbiIocb *pbiRequest)
{
    const AtariDCB *dcb = atariMem.dcb;
    if (structLogs) {
        structLogs->dcb.add(*dcb);
    }
    SidecarSioRecord rec = {};
    rec.unit = dcb->DUNIT;
    rec.command = dcb->DCOMND;
    rec.sector = (uint16_t)(dcb->DAUX2 << 8) | dcb->DAUX1;
    rec.addr = (uint16_t)(dcb->DBUFHI << 8) | dcb->DBUFLO;
    rec.portb = d000Write[0x301];
    rec.d1ff = d000Write[0x1ff];
    rec.path = '-';
    const int result = handleSio(pbiRequest, rec);
    if (structLogs) {
        // As the 6502 will see it once handlePbiRequest() ORs this in
        // (result is 0 on entry); req still holds the request's flags.
        PbiIocb logged = *pbiRequest;
        logged.result |= result;
        structLogs->sidecarPbi.add(logged);
        rec.result = (uint8_t)result;
        rec.dstats = dcb->DSTATS;
        rec.link = (uint8_t)sidecar_.lastLinkStatus();
        structLogs->sidecarSio.add(rec);
    }
    return result;
}

CORE0_HOTSPOTS_ATTR int SidecarHost::handleSio(PbiIocb *pbiRequest, SidecarSioRecord &rec)
{
    AtariDCB *dcb = atariMem.dcb;
    pbiRequest->y = 1;
    pbiRequest->carry = 0;   // assume fail
    lastIoSec = elapsedSec;
    ioCount++;

    const uint8_t command = dcb->DCOMND;
    const bool sends = sioSendsData(command);
    const bool returns = sioReturnsData(command);
    const uint16_t addr = (uint16_t)(dcb->DBUFHI << 8) | dcb->DBUFLO;
    const uint16_t dbyt = (uint16_t)(dcb->DBYTHI << 8) | dcb->DBYTLO;
    const uint16_t len = sends || returns ? (command == 0x53 ? 4 : dbyt) : 0;
    rec.len = len;
    if (len > SioCommand::kDataBytes) {
        dcb->DSTATS = kAtariNak;   // bigger than a link frame (and any sector)
        return RES_FLAG_COMPLETE;
    }

    // As handlePbiRequest2(): use the Atari buffer where the MMU maps it,
    // else bounce through pbiROM[0x400] (copy-in first for a write).
    // The bus stays attached here, so check the live mmuState:
    // mmuStateSaved is only refreshed by disableBus(), and PORTB/$D1FF
    // writes since then have changed the live mapping.
    pbiRequest->copybuf = addr;
    pbiRequest->copylen = len;
    uint8_t *buffer = len ? mmuCheckRangeMapped(mmuState, addr, len) : nullptr;
#ifdef CSIM
    // for csim code coverage, force half IO's to use the copyin path
    static bool alternate = false;
    if (alternate) buffer = nullptr;
    alternate = !alternate;
#endif
    const bool copyRequired = len && buffer == nullptr;
    if (copyRequired) {
        if (sends && (pbiRequest->req & REQ_FLAG_COPYIN) == 0) {
            return RES_FLAG_NEED_COPYIN;
        }
        buffer = &pbiROM[0x400];
    }
    if (len) {
        rec.path = copyRequired ? 'C' : 'M';
        rec.buffer = (uint32_t)(uintptr_t)buffer;
    }

    SioCommand request;
    request.header.flags = 0;
    request.body = *dcb;
    if (sends) {
        memcpy(request.data, buffer, len);
        for (size_t i = 0; i < sizeof(rec.data) && i < len; i++) rec.data[i] = request.data[i];
    }
    SioResponse response;
    if (sidecar_.sio(request, response, kSioTimeoutUs) != SidecarResult::Ok) {
        ++sioFailures_;
        dcb->DSTATS = kAtariTimeout;
        return RES_FLAG_COMPLETE;
    }
    ++sioRequests_;
    dcb->DSTATS = response.body.DSTATS;
    if (response.header.status == 0) {
        return RES_FLAG_COMPLETE;   // the Sidecar's drive failed it: carry clear
    }
    pbiRequest->carry = 1;
    if (!returns) {
        return RES_FLAG_COMPLETE;
    }
    memcpy(buffer, response.data, len);
    for (size_t i = 0; i < sizeof(rec.data) && i < len; i++) rec.data[i] = response.data[i];
    return copyRequired ? RES_FLAG_COPYOUT : RES_FLAG_COMPLETE;
}

CORE0_HOTSPOTS_ATTR bool SidecarHost::checkPbiRequestHook(PbiIocb *pbiRequest)
{
    if (pbiRequest->cmd != kPbiCmdSio) {
        return false;
    }
    const AtariDCB *dcb = atariMem.dcb;
    if ((dcb->DDEVIC & 0x7f) != kDiskDevice) {
        return false;
    }
    const uint8_t unit = dcb->DUNIT;
    return unit < 32 && ((mountedUnits_ >> unit) & 1u) != 0;
}

bool SidecarHost::mount(int unitId, const std::string &diskSpec)
{
    const bool mounted = sidecar_.mount(unitId, diskSpec);
    if (unitId >= 0 && unitId < 32) {
        const uint32_t bit = 1u << unitId;
        if (mounted && !diskSpec.empty()) {
            mountedUnits_ |= bit;
        } else {
            mountedUnits_ &= ~bit;
        }
    }
    return mounted;
}

void SidecarHost::begin()
{
    Sidecar &sidecar = sidecar_;
    if (sidecarTxPin < 0 || sidecarRxPin < 0) {
        printf("Sidecar: host link pins not assigned (pinDefs.h); link disabled\n");
        return;
    }
    // The host is always the clock source (drives BCLK); the Sidecar
    // follows. So the host drives the same two wires (BCLK, its TX) in both
    // modes, and a wired Sidecar is safe in loopback too.
#if CONFIG_A8BB_SIDECAR_LOOPBACK
    // The link hears itself: RX on the TX pin.
    const i2slink_config_t config = {
        .clock = I2SLINK_CLOCK_SOURCE,
        .i2s_port = 0,
        .bclk_gpio = sidecarBclkPin,
        .tx_gpio = sidecarTxPin,
        .rx_gpio = sidecarTxPin,
        .rx_bclk_invert = false,
        .sync_timeout_us = 0,
        .loopback = true,
    };
#else
    const i2slink_config_t config = {
        .clock = I2SLINK_CLOCK_SOURCE,
        .i2s_port = 0,
        .bclk_gpio = sidecarBclkPin,
        .tx_gpio = sidecarTxPin,
        .rx_gpio = sidecarRxPin,
        .rx_bclk_invert = false,
        .sync_timeout_us = 0,
        .loopback = false,
    };
#endif
    if (sidecar.begin(config) != ESP_OK) {
        printf("Sidecar: link init failed\n");
        return;
    }
#if CONFIG_A8BB_SIDECAR_LOOPBACK
    {
        // Startup self-test: a burst of byte-checked echoes.
        auto index = [](SidecarResult result) { return static_cast<int>(result); };
        unsigned counts[index(SidecarResult::Mismatch) + 1] = {};
        const int checks = 200;
        for (int i = 0; i < checks; ++i) {
            ++counts[index(sidecar.loopbackCheck(20000))];
        }
        printf("Sidecar loopback: %d checks: ok=%u timeout=%u linkfault=%u mismatch=%u "
               "(last link status %d)\n", checks, counts[index(SidecarResult::Ok)],
               counts[index(SidecarResult::Timeout)], counts[index(SidecarResult::LinkFault)],
               counts[index(SidecarResult::Mismatch)], (int)sidecar.lastLinkStatus());
    }
#endif
    if (!sidecar.probe(100000)) {
        printf("Sidecar: not present (i2slink status %d)\n", (int)sidecar.lastLinkStatus());
        return;
    }
    // Smoke test: one SIO status command for D1:, answered by the Sidecar's
    // handler (it fails unless the Sidecar has D1: mounted; that's fine).
    SioCommand command = {};
    command.body.DDEVIC = 0x31;
    command.body.DUNIT = 1;
    command.body.DCOMND = 'S';
    SioResponse response;
    const SidecarResult result = sidecar.sio(command, response, 100000);
    if (result == SidecarResult::Ok) {
        printf("Sidecar: present; SIO test answered (D1: status %s, DSTATS %02x)\n",
               response.header.status ? "ok" : "failed", response.body.DSTATS);
    } else {
        printf("Sidecar: present; SIO test FAILED (i2slink status %d)\n",
               (int)sidecar.lastLinkStatus());
    }
}
#endif
