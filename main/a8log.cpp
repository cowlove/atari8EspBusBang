// LOG() on the host app with the Sidecar link (CONFIG_A8BB_LOG_VIA_SIDECAR):
// see components/a8log/include/a8log.h and main/a8log_host.h.
//
// One RAM ring holds the text for the Sidecar. Anyone may append (a cold
// LOG() from any task; later LOGHOT() from core 0's masked loop), in a
// short interrupts-masked section: FreeRTOS runs single-core here, and
// core 1 never logs. Only the link's owner, the setup()/core 0 task, takes
// text out and sends it (SidecarHost::logDrain()), because the link is
// single-threaded:
//   - right away, from a cold LOG() on that task with interrupts enabled
//     (setup(), the interrupts-enabled windows, the end-of-run report);
//   - one frame per PBI request while the bus is hot (handlePbiRequest():
//     the 6502 waits in the stack program then anyway), via
//     a8logDrainHot();
//   - never from the bmon loop's utility pass.
// LOG() from another task (WiFi, telnet) just appends; a later owner
// drain sends it.
#include "a8log.h"
#include "a8log_host.h"

#if defined(CONFIG_A8BB_LOG_VIA_SIDECAR) && CONFIG_A8BB_LOG_VIA_SIDECAR
#ifdef CSIM
#include "csim.h"
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include "esp_attr.h"
#include "sidecarHost.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
DRAM_ATTR int a8logLevel = A8LOG_INFO;
}

namespace {
constexpr uint32_t kRingBytes = 4096;   // a power of two
DRAM_ATTR uint8_t ring[kRingBytes];
DRAM_ATTR volatile uint32_t ringHead = 0, ringTail = 0;   // free-running
DRAM_ATTR uint32_t ringDropped = 0;     // bytes that didn't fit
DRAM_ATTR uint8_t ringLevel = 0;        // most severe level in the ring (0: none)

DRAM_ATTR bool sidecarRoute = false;    // the Sidecar takes the text (a8logSidecarStart())
DRAM_ATTR bool consoleToo = CONFIG_A8BB_LOG_CONSOLE_TOO;
DRAM_ATTR bool draining = false;        // logDrain() is running: appends only
#ifndef CSIM
DRAM_ATTR TaskHandle_t owner = nullptr; // the task that owns the link
#endif

inline uint32_t IRAM_ATTR maskInterrupts()
{
#ifdef CSIM
    return 0;
#else
    return portSET_INTERRUPT_MASK_FROM_ISR();
#endif
}

inline void IRAM_ATTR restoreInterrupts(uint32_t old)
{
#ifdef CSIM
    (void)old;
#else
    portCLEAR_INTERRUPT_MASK_FROM_ISR(old);
#endif
}

// The calling context may send (drain) now: the link's owner, interrupts
// enabled, not inside a drain already.
bool canDrainCold()
{
    if (draining) {
        return false;
    }
#ifdef CSIM
    return !csim_in_sidecar();
#else
    uint32_t ps;
    __asm__ volatile("rsr.ps %0" : "=a"(ps));
    return (ps & 0xf) == 0 && xTaskGetCurrentTaskHandle() == owner;
#endif
}
}  // namespace

IRAM_ATTR size_t a8logRingPut(const char *text, size_t len, int level)
{
    const uint32_t old = maskInterrupts();
    const uint32_t room = kRingBytes - (ringHead - ringTail);
    const size_t n = len < room ? len : room;
    uint32_t head = ringHead;
    for (size_t i = 0; i < n; ++i) {
        ring[head++ & (kRingBytes - 1)] = (uint8_t)text[i];
    }
    ringHead = head;
    ringDropped += len - n;
    if (n && level > 0 && (ringLevel == 0 || level < ringLevel)) {
        ringLevel = (uint8_t)level;
    }
    restoreInterrupts(old);
    return n;
}

IRAM_ATTR size_t a8logRingPeek(const uint8_t **data)
{
    const uint32_t tail = ringTail;
    const uint32_t avail = ringHead - tail;
    const uint32_t at = tail & (kRingBytes - 1);
    *data = &ring[at];
    return avail < kRingBytes - at ? avail : kRingBytes - at;   // up to the wrap
}

IRAM_ATTR void a8logRingConsume(size_t n)
{
    ringTail += n;   // one consumer (the owner); appends only move the head
}

IRAM_ATTR uint8_t a8logRingTakeLevel()
{
    const uint32_t old = maskInterrupts();
    const uint8_t level = ringLevel;
    ringLevel = 0;
    restoreInterrupts(old);
    return level;
}

uint32_t a8logRingDropped() { return ringDropped; }

IRAM_ATTR bool a8logSidecarRoute() { return sidecarRoute; }

void a8logSidecarStart(bool sidecarCanPrint)
{
    bool want = true;
    bool console = CONFIG_A8BB_LOG_CONSOLE_TOO;
#ifdef CSIM
    // csim's Sidecar prints to the same stdout: one route by default.
    const char *route = getenv("CSIM_LOG_ROUTE");
    want = route && (strcmp(route, "sidecar") == 0 || strcmp(route, "both") == 0);
    console = !route || strcmp(route, "sidecar") != 0;
#else
    owner = xTaskGetCurrentTaskHandle();
#endif
    consoleToo = console;
    sidecarRoute = want && sidecarCanPrint;
    printf("LOG: %s%s\n",
           sidecarRoute ? "to the Sidecar's console" : "console only",
           sidecarRoute ? (consoleToo ? " and this one" : " only") :
           (want ? " (the Sidecar can't print SerialOut)" : ""));
}

// Hot-safe (from logDrain() on a PBI request): no printing here; the next
// cold LOG() reports it.
DRAM_ATTR static const char *stopReason = nullptr;
DRAM_ATTR static uint32_t stopLost = 0;

IRAM_ATTR void a8logSidecarStop(const char *why)
{
    if (!sidecarRoute) {
        return;
    }
    sidecarRoute = false;
    const uint32_t old = maskInterrupts();
    stopLost = ringHead - ringTail;
    ringTail = ringHead;
    restoreInterrupts(old);
    stopReason = why;
    consoleToo = true;
}

IRAM_ATTR void a8logDrainHot()
{
    if (!sidecarRoute || draining || ringHead == ringTail) {
        return;
    }
    draining = true;
    sidecarHost.logDrain(1);
    draining = false;
}

void a8logFlush()
{
    if (!sidecarRoute || !canDrainCold()) {
        return;
    }
    draining = true;
    sidecarHost.logDrain(-1);
    draining = false;
}

void a8logReport()
{
    if (sidecarHost.logFrames() == 0 && sidecarHost.logFailures() == 0 && ringDropped == 0) {
        return;
    }
    const uint32_t frames = sidecarHost.logFrames();
    LOG("LOG: %u SerialOut frames to the Sidecar (mean %u us, max %u us each), %u failed sends, "
        "%u bytes dropped (ring full)%s\n",
        (unsigned)frames, frames ? (unsigned)(sidecarHost.logTicks() / frames / 240) : 0,
        (unsigned)(sidecarHost.logTicksMax() / 240), (unsigned)sidecarHost.logFailures(),
        (unsigned)ringDropped, sidecarRoute ? "" : "; route off");
    a8logFlush();
}

extern "C" int a8log_printf(int level, const char *format, ...)
{
    if (level > a8logLevel) {
        return 0;
    }
    va_list args;
    va_start(args, format);
    int n = 0;
#ifdef CSIM
    if (csim_in_sidecar()) {
        // Code both apps share (disk_image, sidecar_link), run by csim's
        // Sidecar: its own console.
        n = vprintf(format, args);
        va_end(args);
        return n;
    }
#endif
    if (stopReason) {
        const char *why = stopReason;
        stopReason = nullptr;
        printf("LOG: Sidecar route off (%s); %u bytes not sent; console only from here\n", why,
               (unsigned)stopLost);
    }
    if (!sidecarRoute) {
        n = vprintf(format, args);
        va_end(args);
        return n;
    }
    if (consoleToo) {
        va_list copy;
        va_copy(copy, args);
        n = vprintf(format, copy);
        va_end(copy);
    }
    n = sidecarHost.log(level, format, args);
    va_end(args);
    // Send at line ends or once a frame's worth is waiting: not one link
    // round trip per "%02x " of a hex dump.
    const uint32_t pending = ringHead - ringTail;
    const bool lineDone = pending && ring[(ringHead - 1) & (kRingBytes - 1)] == '\n';
    if ((lineDone || pending >= SerialOutCommand::kDataBytes) && canDrainCold()) {
        draining = true;
        sidecarHost.logDrain(-1);
        draining = false;
    }
    return n;
}

#endif  // CONFIG_A8BB_LOG_VIA_SIDECAR
