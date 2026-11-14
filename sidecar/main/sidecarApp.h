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

// Run one console command line (fetch test, ping, ...): the USB console on
// the board, CSIM_SIDECAR_CMD in csim.
void sidecarAppCommand(const char *line);

