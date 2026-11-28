// LOG() and LOGHOT() on the host app with the Sidecar link
// (A8BB_LOG_VIA_SIDECAR): see components/a8log/include/a8log.h and
// main/a8log_host.h.
//
// Synchronous: a line goes to the Sidecar from the call that writes it
// (SidecarHost::log() / logHot() -> logPut() -> Sidecar::serialOut()),
// line-buffered as stdout is: a frame goes out at each newline, or when the
// frame (281 bytes) is full. Only core 0's task logs: setup(), then the
// core 0 loop and its interrupts-enabled windows; it has no concurrent
// callers there. A cold LOG() from any other task only prints on this
// board's console. logPut() isn't re-entrant: a LOG()/LOGHOT() from inside
// it, or from inside any other link call (sidecarHost and below), is not
// sent; it only prints here if it's a LOG() and the console tee is on.
#include "a8log.h"
#include "a8log_host.h"

#if defined(A8BB_LOG_VIA_SIDECAR) && A8BB_LOG_VIA_SIDECAR
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
DRAM_ATTR bool sidecarRoute = false;   // the Sidecar takes the text (a8logSidecarStart())
DRAM_ATTR bool consoleToo = A8BB_LOG_CONSOLE_TOO;
#ifndef CSIM
DRAM_ATTR TaskHandle_t owner = nullptr;   // core 0's task: the only one that sends
#endif
// From a8logSidecarStop(), which may run hot: reported by the next cold LOG().
DRAM_ATTR const char *stopReason = nullptr;

// A cold LOG() may send from here: core 0's task (csim: not the Sidecar's
// coroutine).
bool onOwnerTask()
{
#ifdef CSIM
    return !csim_in_sidecar();
#else
    return xTaskGetCurrentTaskHandle() == owner;
#endif
}
}  // namespace

IRAM_ATTR bool a8logSidecarRoute() { return sidecarRoute; }

void a8logSidecarStart(bool sidecarCanPrint)
{
    bool want = true;
    bool console = A8BB_LOG_CONSOLE_TOO;
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

// Hot-safe (logPut() can run hot): no printing here.
IRAM_ATTR void a8logSidecarStop(const char *why)
{
    if (!sidecarRoute) {
        return;
    }
    sidecarRoute = false;
    stopReason = why;
    consoleToo = true;
}

void a8logFlush()
{
    if (sidecarRoute && onOwnerTask()) {
        sidecarHost.logFlush();
    }
}

// LOGHOT() (a8log.h): core 0, hot-safe all the way (nanoprintf in IRAM,
// SidecarHost::logHot()).
extern "C" IRAM_ATTR void a8log_hot(int level, const char *format, ...)
{
    if (level > a8logLevel || !sidecarRoute) {
        return;   // no route: nowhere to go, nothing hot may print here
    }
    va_list args;
    va_start(args, format);
    sidecarHost.logHot(level, format, args);
    va_end(args);
}

void a8logReport()
{
    const uint32_t frames = sidecarHost.logFrames();
    if (frames == 0 && sidecarHost.logFailures() == 0) {
        return;
    }
    LOG("LOG: %u SerialOut frames to the Sidecar (mean %u us, max %u us each), %u failed sends, "
        "%u lines not sent (re-entered)%s\n",
        (unsigned)frames, frames ? (unsigned)(sidecarHost.logTicks() / frames / 240) : 0,
        (unsigned)(sidecarHost.logTicksMax() / 240), (unsigned)sidecarHost.logFailures(),
        (unsigned)sidecarHost.logReentered(), sidecarRoute ? "" : "; route off");
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
    if (stopReason) {
        const char *why = stopReason;
        stopReason = nullptr;
        printf("LOG: Sidecar route off (%s); console only from here\n", why);
    }
    const bool send = sidecarRoute && onOwnerTask();
    if (!send || consoleToo) {
        va_list copy;
        va_copy(copy, args);
        n = vprintf(format, copy);
        va_end(copy);
    }
    if (send) {
        n = sidecarHost.log(level, format, args);
    }
    va_end(args);
    return n;
}

#endif  // A8BB_LOG_VIA_SIDECAR

#if defined(A8LOG_BOOT_STAMP_MS) && A8LOG_BOOT_STAMP_MS > 0
#ifdef CSIM
#include "csim.h"
#else
#include "esp_timer.h"
#endif
#include <string.h>

// The "[%05u] " uptime prefix of LOG() lines in the first
// A8LOG_BOOT_STAMP_MS ms (a8log.h). Cold, any task; a race between two
// tasks' lines can only misplace a stamp.
extern "C" void a8log_bootStamp(int level, const char *format)
{
    static bool over = false;
    static bool atLineStart = true;
    if (over)
        return;
#ifdef CSIM
    // csim: the Sidecar app's lines (its coroutine) go unstamped, as on its
    // own board. The clock is esp_timer_get_time()'s (CCOUNT / 240, from the
    // process's start; the hardware's also counts the ROM and bootloader),
    // read without its coroutine yield, so no other coroutine prints
    // between the stamp and its line.
    if (csim_in_sidecar())
        return;
    const unsigned ms = (unsigned)(CSIM_PEEK_CCOUNT64() / 240000U);
#else
    const unsigned ms = (unsigned)(esp_timer_get_time() / 1000);
#endif
    if (ms >= A8LOG_BOOT_STAMP_MS) {
        over = true;
        return;
    }
    if (atLineStart)
        A8LOG_EMIT(level, "[%05u] ", ms);
    const size_t n = format ? strlen(format) : 0;
    if (n)
        atLineStart = format[n - 1] == '\n';
}
#endif  // A8LOG_BOOT_STAMP_MS
