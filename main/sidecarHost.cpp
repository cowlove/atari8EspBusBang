#include "sidecarHost.h"

#if CONFIG_A8BB_SIDECAR_ENABLE
#include "pinDefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#ifndef CSIM
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#endif

#include "asmDefs.h"   // REQ_FLAG_*, RES_FLAG_*
#include "main.h"      // atariMem (the DCB), ioCount, lastIoSec, elapsedSec
#include "mmu.h"       // mmuCheckRangeMapped(), mmuState, pbiROM
#include "pbi.h"       // PbiIocb
#include "log.h"       // structLogs: fixed rings, safe with core 1 hot

DRAM_ATTR SidecarHost sidecarHost;

namespace {
constexpr uint8_t kPbiCmdSio = 7;       // PbiIocb::cmd for SIO (see handlePbiRequest2())
constexpr uint8_t kDiskDevice = 0x31;   // DDEVIC for D1:...D8: (low 7 bits)

// How long setup() waits for the Sidecar to answer (begin()).
static constexpr uint32_t kSidecarProbeUs = 3000000;
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
    // The diagnostics (the SIO record, the DCB copy) run only with the struct
    // logs on (structLogsInit()): reading d000Write and copying the data
    // bytes touches core 1's hot banks while it runs the bus. With them off,
    // handleSio() gets no record and the record is never even cleared.
    SidecarSioRecord rec;
    SidecarSioRecord *log = nullptr;
    if (structLogs) {
        log = &rec;
        rec = {};
        structLogs->dcb.add(*dcb);
        rec.unit = dcb->DUNIT;
        rec.command = dcb->DCOMND;
        rec.sector = (uint16_t)(dcb->DAUX2 << 8) | dcb->DAUX1;
        rec.addr = (uint16_t)(dcb->DBUFHI << 8) | dcb->DBUFLO;
        rec.portb = d000Write[0x301];
        rec.d1ff = d000Write[0x1ff];
        rec.path = '-';
    }
    const int result = handleSio(pbiRequest, log);
    if (log) {
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

CORE0_HOTSPOTS_ATTR int SidecarHost::handleSio(PbiIocb *pbiRequest, SidecarSioRecord *log)
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
    if (log) {
        log->len = len;
    }
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
    if (log && len) {
        log->path = copyRequired ? 'C' : 'M';
        log->buffer = (uint32_t)(uintptr_t)buffer;
    }

    SioCommand request;
    request.header.flags = 0;
    request.body = *dcb;
    if (sends) {
        memcpy(request.data, buffer, len);
        if (log) {
            for (size_t i = 0; i < sizeof(log->data) && i < len; i++) log->data[i] = request.data[i];
        }
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
    if (log) {
        for (size_t i = 0; i < sizeof(log->data) && i < len; i++) log->data[i] = response.data[i];
    }
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

static const char *objectName(SidecarObject object)
{
    switch (object) {
    case SidecarObject::Firmware: return "Firmware";
    case SidecarObject::Core: return "Core";
    case SidecarObject::Test: return "Test";
    }
    return "?";
}

void SidecarHost::fetchReport(SidecarObject object)
{
    const int64_t start = esp_timer_get_time();
    uint32_t wrong = 0;
    const SidecarFetchResult r = sidecarFetch(sidecar_, object,
        [&](uint32_t offset, const uint8_t *data, uint16_t length) {
            if (object == SidecarObject::Test) {
                for (uint16_t i = 0; i < length; ++i)
                    wrong += data[i] != sidecarTestObjectByte(offset + i);
            }
            return true;
        });
    const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    printf("Sidecar fetch %s: link %d, status %d, %u of %u bytes, crc %08x, version %08x, %s%s, "
           "%d ms (%.1f kB/s)\n", objectName(object), (int)r.link, (int)r.status,
           (unsigned)r.received, (unsigned)r.size, (unsigned)r.crc, (unsigned)r.version,
           r.consistent && r.crcMatches ? "OK" : "INCONSISTENT",
           object == SidecarObject::Test ? (wrong ? ", PATTERN WRONG" : ", pattern ok") : "",
           ms, ms ? r.received / 1.024 / ms : 0.0);
}

#ifndef CSIM
// OTA from the Sidecar, in diagnostic mode (setup(), before core 1 starts:
// flash writes stall nothing). Pulls the Firmware object straight into the
// next app slot (esp_ota_write() as each chunk arrives), keeps it only if
// the fetch was complete and consistent (the same token before and after,
// and the bytes' CRC32 equals it) and esp_ota_end() accepts the image (its
// own checks), then makes it the boot slot and restarts. Returns only on
// failure, with the running app untouched.
bool SidecarHost::installFirmware()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    // With one app slot (master's 8 MB layout) the "next" slot is the
    // running one: no update slot either.
    if (!target || target == running) {
        printf("OTA: no update slot (this partition table has no ota_1)\n");
        return false;
    }
    printf("OTA: running %s (0x%06x); fetching Firmware from the Sidecar into %s (0x%06x, %u bytes)\n",
           running ? running->label : "?", running ? (unsigned)running->address : 0, target->label,
           (unsigned)target->address, (unsigned)target->size);
    fflush(stdout);
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        printf("OTA: esp_ota_begin: %s\n", esp_err_to_name(err));
        return false;
    }
    const int64_t start = esp_timer_get_time();
    esp_err_t writeErr = ESP_OK;
    const SidecarFetchResult r = sidecarFetch(sidecar_, SidecarObject::Firmware,
        [&](uint32_t, const uint8_t *data, uint16_t length) {
            writeErr = esp_ota_write(ota, data, length);
            return writeErr == ESP_OK;
        });
    const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    printf("OTA: fetch: link %d, status %d, %u of %u bytes, crc %08x, version %08x, %s, %d ms "
           "(%.1f kB/s)%s%s\n", (int)r.link, (int)r.status, (unsigned)r.received, (unsigned)r.size,
           (unsigned)r.crc, (unsigned)r.version, r.consistent && r.crcMatches ? "OK" : "INCONSISTENT",
           ms, ms ? r.received / 1.024 / ms : 0.0, writeErr == ESP_OK ? "" : ", write: ",
           writeErr == ESP_OK ? "" : esp_err_to_name(writeErr));
    if (r.link != SidecarResult::Ok || r.status != SidecarReadStatus::Ok || !r.consistent ||
        !r.crcMatches || writeErr != ESP_OK || r.received == 0) {
        esp_ota_abort(ota);
        printf("OTA: FAILED; keeping %s\n", running ? running->label : "the running app");
        return false;
    }
    err = esp_ota_end(ota);   // validates the image
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(target);
    }
    if (err != ESP_OK) {
        printf("OTA: image rejected: %s; keeping %s\n", esp_err_to_name(err),
               running ? running->label : "the running app");
        return false;
    }
    printf("OTA: installed %u bytes into %s; restarting into it\n", (unsigned)r.received, target->label);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}
#endif

// First 4 bytes of a SHA-256, little endian: a version token.
static uint32_t shaToken(const uint8_t *sha)
{
    return sha[0] | (sha[1] << 8) | (sha[2] << 16) | ((uint32_t)sha[3] << 24);
}

#ifndef CSIM
// The core dump in flash, served as the Core object in diagnostic mode only
// (Sidecar::pollDiagnostic(): bus detached, interrupts on, so flash reads
// are fine; core 1 keeps running as it does for any detached flash I/O).
static uint32_t coreFlashAddr;
static bool readCore(uint32_t offset, void *buffer, uint32_t length)
{
    return esp_flash_read(nullptr, buffer, coreFlashAddr + offset, length) == ESP_OK;
}
#endif

// The boot report (BootStatus, sidecar.h). From begin(): before core 1
// starts, so reading flash (the core dump's checksum) is safe. Cheap: the
// core dump's header and its stored SHA-256 (32 bytes at the end), no
// rehash of the whole dump.
void SidecarHost::sendBootStatus()
{
    BootStatusCommand command = {};
    SidecarBootStatusBody &body = command.body;
#ifdef CSIM
    body.resetReason = 1;   // ESP_RST_POWERON
#else
    body.resetReason = static_cast<uint8_t>(esp_reset_reason());
    if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                 nullptr)) {
        body.flags |= static_cast<uint8_t>(SidecarBootFlag::CorePartition);
        size_t addr = 0, size = 0;
        uint8_t sha[32];
        if (esp_core_dump_image_get(&addr, &size) == ESP_OK && size > sizeof(sha) &&
            esp_flash_read(nullptr, sha, addr + size - sizeof(sha), sizeof(sha)) == ESP_OK) {
            body.flags |= static_cast<uint8_t>(SidecarBootFlag::CorePresent);
            body.coreSize = size;
            body.coreVersion = shaToken(sha);
            coreFlashAddr = addr;
            sidecar_.setCoreObject(size, body.coreVersion, readCore);
        }
    }
    if (esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                 nullptr)) {
        body.flags |= static_cast<uint8_t>(SidecarBootFlag::OtaSlot);
    }
    body.appVersion = shaToken(esp_app_get_description()->app_elf_sha256);
#endif
    strncpy(body.git, GIT_VERSION, sizeof(body.git) - 1);
    // The reply carries the Sidecar's decision: the diagnostic flags to
    // enter (setup() then goes into diagnostic mode instead of booting).
    BootStatusResponse response;
    const SidecarResult r = sidecar_.transact(SidecarCommand::BootStatus, &command, &response, 1000000);
    if (r == SidecarResult::Ok) {
        sidecar_.setDiagnosticFlags(response.body.diagnostic);
    }
#ifndef CSIM
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *slot = running ? running->label : "?";
#else
    const char *slot = "csim";
#endif
    printf("Sidecar: boot status %s: reset %d, flags %02x, core %u bytes %08x, app %08x (%s); "
           "diagnostic %02x\n", r == SidecarResult::Ok ? "answered" : "NOT ANSWERED", body.resetReason,
           body.flags, (unsigned)body.coreSize, (unsigned)body.coreVersion, (unsigned)body.appVersion,
           slot, sidecar_.diagnosticFlags());
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
    // A build with the link expects a Sidecar. Development model: wait a
    // little (it may still be booting: up to ~2 s after a common power-on),
    // complain, and boot normally without it; the test farm's boot.sh
    // brings the Sidecar up first. Production boards will have the Sidecar
    // boot main deliberately (no race), and a missing Sidecar will likely
    // be fatal there.
    if (!sidecar.probe(kSidecarProbeUs, 50000)) {
        printf("Sidecar: EXPECTED A SIDECAR, none answered in %d ms (i2slink status %d); "
               "booting without it\n", (int)(kSidecarProbeUs / 1000), (int)sidecar.lastLinkStatus());
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
    sendBootStatus();
#ifdef CSIM
    // csim: CSIM_LINK_TEST=1 fetches the Sidecar's objects at startup.
    if (getenv("CSIM_LINK_TEST")) {
        fetchReport(SidecarObject::Test);
        fetchReport(SidecarObject::Firmware);
    }
#endif
}
#endif
