#include "sidecar.h"
#include "esp_attr.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#ifndef CSIM
#include "esp_cpu.h"
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
}  // namespace

// ---------------------------------------------------------------------------
// Sidecar (host side)
// ---------------------------------------------------------------------------

esp_err_t Sidecar::begin(const i2slink_config_t &config)
{
    if (link_) {
        return ESP_ERR_INVALID_STATE;
    }
    present_ = false;
    loopback_ = config.loopback;
    // A random start, so a response left on the wire from before this end
    // restarted can't match a new command's sequence.
    nextSequence_ = esp_random();
    return i2slink_init(&link_, &config);
}

void Sidecar::end()
{
    i2slink_deinit(link_);
    link_ = nullptr;
    present_ = false;
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
        // Each try is a new write, so a Sidecar that dropped the previous
        // one (its first command after a restart) sees a NEW command.
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
    return transact(SidecarCommand::Nop, command.header, &command, &response, timeoutUs);
}

IRAM_ATTR SidecarResult Sidecar::sio(SioCommand &command, SioResponse &response, uint32_t timeoutUs)
{
    return transact(SidecarCommand::Sio, command.header, &command, &response, timeoutUs);
}

IRAM_ATTR SidecarResult Sidecar::cio(CioCommand &command, CioResponse &response, uint32_t timeoutUs)
{
    return transact(SidecarCommand::Cio, command.header, &command, &response, timeoutUs);
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
    const SidecarResult result =
        transact(SidecarCommand::Mount, command.header, &command, &response, timeoutUs);
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
    for (size_t index = sizeof(SidecarHeader); index < sizeof(command); ++index) {
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
    // A plain loop: memcmp() isn't on the hot-call check's ROM allowlist.
    const uint8_t *sent = bytes;
    const uint8_t *echoed = reinterpret_cast<const uint8_t *>(&response);
    for (size_t index = 0; index < sizeof(command); ++index) {
        if (sent[index] != echoed[index]) {
            return SidecarResult::Mismatch;
        }
    }
    return SidecarResult::Ok;
}

IRAM_ATTR SidecarResult Sidecar::transact(SidecarCommand command, SidecarHeader &header,
                                          const void *commandBytes, void *responseBytes, uint32_t timeoutUs)
{
    if (!link_) {
        lastStatus_ = I2SLINK_NO_LOCK;
        return SidecarResult::LinkFault;
    }
    header.command = static_cast<uint8_t>(command);
    header.sequence = nextSequence_++;
    header.status = 0;
    // One write: the link resends it every wire cycle until the next write.
    i2slink_write(link_, static_cast<const uint8_t *>(commandBytes));

    const uint32_t start = ticksNow();
    // The last link fault seen, if any: reported if time runs out.
    i2slink_status_t fault = I2SLINK_NEW;
    for (;;) {
        const uint32_t remaining = remainingUs(start, timeoutUs);
        if (remaining == 0) {
            if (fault != I2SLINK_NEW) {
                lastStatus_ = fault;
                return SidecarResult::LinkFault;
            }
            lastStatus_ = I2SLINK_TIMEOUT;
            return SidecarResult::Timeout;
        }
        // The wait happens in i2slink_read(), paced and memory-only.
        lastStatus_ = i2slink_read(link_, rx_, nullptr, remaining);
        if (lastStatus_ == I2SLINK_TIMEOUT) {
            if (fault != I2SLINK_NEW) {
                lastStatus_ = fault;   // timed out after a fault: report the fault
                return SidecarResult::LinkFault;
            }
            return SidecarResult::Timeout;
        }
        if (lastStatus_ != I2SLINK_NEW) {
            // TORN, CRC, NO_LOCK or STALLED, for sync_timeout_us. The link
            // keeps resending the command and stale replies are ignored by
            // sequence, so a fault that clears in time costs nothing: keep
            // waiting until the deadline.
            fault = lastStatus_;
            continue;
        }
        SidecarHeader reply;
        memcpy(&reply, rx_, sizeof(reply));
        if (reply.sequence == header.sequence && reply.command == header.command) {
            memcpy(responseBytes, rx_, I2SLINK_PAYLOAD_BYTES);
            return SidecarResult::Ok;
        }
        // A response to some other (earlier) command: keep waiting.
    }
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
    if (link_) {
        return ESP_ERR_INVALID_STATE;
    }
    primed_ = false;
    retryAfterUs_ = 0;
    return i2slink_init(&link_, &config);
}

void SidecarService::end()
{
    i2slink_deinit(link_);
    link_ = nullptr;
}

bool SidecarService::service()
{
    if (!link_ || !i2slink_hint(link_)) {
        return false;
    }
    if (retryAfterUs_ && esp_timer_get_time() < retryAfterUs_) {
        return false;
    }
    lastStatus_ = i2slink_read(link_, rx_, nullptr, 0);
    if (lastStatus_ != I2SLINK_NEW) {
        // TIMEOUT: the hint was wrong, nothing new. Anything else is a link
        // fault: try again later.
        retryAfterUs_ = lastStatus_ == I2SLINK_TIMEOUT ? 0 : esp_timer_get_time() + kFaultBackoffUs;
        return false;
    }
    retryAfterUs_ = 0;
    if (!primed_) {
        primed_ = true;   // possibly run before a restart: drop it
        return false;
    }

    SidecarHeader header;
    memcpy(&header, rx_, sizeof(header));
    header.status = 0;
    memcpy(tx_, rx_, I2SLINK_PAYLOAD_BYTES);   // responses mirror commands for now
    memcpy(tx_, &header, sizeof(header));      // the response's status starts at 0
    switch (static_cast<SidecarCommand>(header.command)) {
    case SidecarCommand::Nop:
	handleNop();
        break;   // the echo is the answer
    case SidecarCommand::Sio:
        handleSio(*reinterpret_cast<const SioCommand *>(rx_), *reinterpret_cast<SioResponse *>(tx_));
        break;
    case SidecarCommand::Cio:
        handleCio(*reinterpret_cast<const CioCommand *>(rx_), *reinterpret_cast<CioResponse *>(tx_));
        break;
    case SidecarCommand::Mount:
        handleMount(*reinterpret_cast<const MountCommand *>(rx_),
                    *reinterpret_cast<MountResponse *>(tx_));
        break;
    default:
        return false;   // unknown command: no response; the host times out
    }
    // Always echo the header; keep the status the handler set.
    header.status = reinterpret_cast<const SidecarHeader *>(tx_)->status;
    memcpy(tx_, &header, sizeof(header));
    i2slink_write(link_, tx_);
    return true;
}
