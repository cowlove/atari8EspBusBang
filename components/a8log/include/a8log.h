#pragma once
// a8log: LOG(), what the project's code calls instead of printf().
//
//   LOG("io: %d\n", n);                 // a printf(): same arguments, same result
//   LOGL(A8LOG_WARN, "late by %d\n", t);  // with a level
//
// For now LOG() *is* printf() everywhere (the host app, the Sidecar app,
// csim): putting it in first changes nothing. What it's for (the host app
// with the Sidecar link):
//   - routing the host's log lines to the Sidecar, which prints them on its
//     console (SidecarHost::log(), Sidecar::serialOut());
//   - a hot-bus-safe variant, LOGHOT(), for core 0 code that runs with
//     interrupts masked, where printf() aborts;
//   - log levels.
//
// Levels: a sketch, so far. Everything logs at A8LOG_INFO (LOG()); LOGL()
// takes a level. A8LOG_MIN_LEVEL (compile time, default A8LOG_VERBOSE: all)
// drops calls above it at compile time; the levels aren't sorted out in the
// existing calls yet. The numbering follows esp_log.h's (ESP_LOG_ERROR = 1
// ... ESP_LOG_VERBOSE = 5).
//
// C and C++. Not named log.h: main/log.h is the StructLog ring.

#include <stdio.h>

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
#define A8LOG_EMIT(level, ...) printf(__VA_ARGS__)

// An expression with printf()'s value (characters written; 0 when the
// level drops it), so LOG() fits wherever printf() did.
#define LOGL(level, ...) ((level) <= A8LOG_MIN_LEVEL ? A8LOG_EMIT((level), __VA_ARGS__) : 0)
#define LOG(...) LOGL(A8LOG_INFO, __VA_ARGS__)
