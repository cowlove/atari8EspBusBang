#pragma once

// The Sidecar app proper: its SidecarService (the SIO/CIO handlers) and one
// iteration of its outer loop. Hardware-independent, so the same code runs
// on the Sidecar ESP32 (main.cpp's core-1 loop) and inside CSIM (the same
// loop on a coroutine of its own, csim/a800/csim_sidecar.cpp).

#include "i2slink.h"

// The Sidecar's link: it drives the clock. Pins of the bench Sidecar board
// (the i2sDmaSenderBench SLAVE); the host side is the follower.
extern const i2slink_config_t sidecarLinkConfig;

// Start the link and the service. Call once.
void sidecarAppBegin();

// One pass of the outer loop: service at most one command, plus the
// Sidecar's other periodic work. Never blocks.
void sidecarAppPoll();

// The Sidecar's link end (stats, csim's end-of-run report).
i2slink_t *sidecarAppLink();
// The response-time histogram (SidecarPeer::responseTimes()), printed: csim's end-of-run report.
void sidecarAppReportTiming();

// Run one console command line (fetch test, ping, ...): the USB console on
// the board, CSIM_SIDECAR_CMD in csim.
void sidecarAppCommand(const char *line);


// The web console (sidecarWeb.cpp's /console/...). Everything the Sidecar
// prints (main's proxied output included) is mirrored into a ring buffer,
// read by offset; web command lines are queued and run by the service loop
// as if typed on the USB console.
#include <cstddef>
#include <cstdint>
#include <string>
// Mirror stdout into the ring (the board: app_main(), first; csim: none,
// the ring stays empty).
void sidecarConsoleTeeBegin();
// The console's bytes from offset `since` (since the Sidecar started), up
// to max into out; returns how many. *next: the offset after them; *lost:
// bytes after `since` already overwritten (skipped).
size_t sidecarConsoleRead(uint64_t since, char *out, size_t max, uint64_t *next, uint64_t *lost);
uint64_t sidecarConsoleTotal();
// Queue one command line from the web; false (with why) if refused: too
// long, the queue full, or a USB-only command (recv/send).
bool sidecarConsoleQueue(const std::string &line, std::string *why);
