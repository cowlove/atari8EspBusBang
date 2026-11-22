#pragma once
// a8log: LOG(), what the project's code calls instead of printf().
//
//   LOG("io: %d\n", n);                 // a printf(): same arguments, same result
//   LOGL(A8LOG_WARN, "late by %d\n", t);  // with a level
//
// LOG() is printf() in the Sidecar app, in host builds without the
// Sidecar link, and wherever A8BB_LOG_VIA_SIDECAR is off. With it
// (the host app with the Sidecar link, main/a8log.cpp), LOG() is
// a8log_printf(): the text is also sent to the Sidecar, right away,
// line-buffered (SidecarHost::log(), Sidecar::serialOut()), which prints it
// on its own console; with A8BB_LOG_CONSOLE_TOO it's printed here as
// well. Until the Sidecar is up (and on a Sidecar too old for SerialOut)
// it's plain printf(). Only core 0's task sends (setup(), the core 0
// loop's cold paths); a LOG() from another task only prints here.
//
// LOGHOT(): for core 0 code that runs with core 1 hot (interrupts masked,
// no flash), where printf() and LOG() can't run. It formats with an IRAM
// formatter (nanoprintf, main/a8logFmt.cpp) and sends at once
// (SidecarHost::logHot()): a line costs its SerialOut round trip, ~2.2 ms
// on the bench pair, right there in the hot code. The format must be a
// string literal (LOGHOT puts it in DRAM) and %s arguments must point to
// RAM. No floats. Lines are cut at 159 characters. Only with the Sidecar
// route: in other builds, and while the route is off, LOGHOT() does
// nothing. Not on this board's console either (nothing hot may print),
// only the Sidecar's.
//
// Not re-entrant: a LOG()/LOGHOT() from inside the send path or any other
// link call (SidecarHost and below) is not sent (counted). Future: keep
// such lines, or the most severe levels, in a short SRAM trace.
//
// Levels: a sketch, so far. Everything logs at A8LOG_INFO (LOG()); LOGL()
// takes a level. A8LOG_MIN_LEVEL (compile time, default A8LOG_VERBOSE: all)
// drops calls above it at compile time; the levels aren't sorted out in the
// existing calls yet. The numbering follows esp_log.h's (ESP_LOG_ERROR = 1
// ... ESP_LOG_VERBOSE = 5).
//
// C and C++. Not named log.h: main/log.h is the StructLog ring.

#include <stdio.h>
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#define A8LOG_NONE    0
#define A8LOG_ERROR   1
#define A8LOG_WARN    2
#define A8LOG_INFO    3
#define A8LOG_DEBUG   4
#define A8LOG_VERBOSE 5

#ifndef A8LOG_MIN_LEVEL
#define A8LOG_MIN_LEVEL A8LOG_VERBOSE
#endif

// How a line that passes the level check goes out.
#if defined(A8BB_LOG_VIA_SIDECAR) && A8BB_LOG_VIA_SIDECAR
#ifdef __cplusplus
extern "C" {
#endif
// main/a8log.cpp: the console and/or the Sidecar. Cold: from any task.
int a8log_printf(int level, const char *format, ...) __attribute__((format(printf, 2, 3)));
// The runtime level: lines above it are dropped (default A8LOG_INFO).
extern int a8logLevel;
#ifdef __cplusplus
}
#endif
#define A8LOG_EMIT(level, ...) a8log_printf((level), __VA_ARGS__)
#include "esp_attr.h"   // DRAM_STR
#ifdef __cplusplus
extern "C"
#endif
void a8log_hot(int level, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define LOGHOTL(level, fmt, ...)                                     \
    do {                                                             \
        if ((level) <= A8LOG_MIN_LEVEL)                              \
            a8log_hot((level), DRAM_STR(fmt), ##__VA_ARGS__);        \
    } while (0)
#else
#define A8LOG_EMIT(level, ...) printf(__VA_ARGS__)
#define LOGHOTL(level, fmt, ...) do { } while (0)
#endif
#define LOGHOT(fmt, ...) LOGHOTL(A8LOG_INFO, fmt, ##__VA_ARGS__)

// An expression with printf()'s value (characters written; 0 when the
// level drops it), so LOG() fits wherever printf() did.
#define LOGL(level, ...) ((level) <= A8LOG_MIN_LEVEL ? A8LOG_EMIT((level), __VA_ARGS__) : 0)
#define LOG(...) LOGL(A8LOG_INFO, __VA_ARGS__)
