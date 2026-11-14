#pragma once
// The host app's LOG() plumbing with the Sidecar link (main/a8log.cpp;
// CONFIG_A8BB_LOG_VIA_SIDECAR). LOG() itself is components/a8log.
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#if defined(CONFIG_A8BB_LOG_VIA_SIDECAR) && CONFIG_A8BB_LOG_VIA_SIDECAR
// The ring of text for the Sidecar. Put: any task or core 0's masked loop
// (IRAM, memory only); returns the bytes stored (the rest is counted as
// dropped). Peek/consume/take-level: the link's owner only (logDrain()).
size_t a8logRingPut(const char *text, size_t len, int level);
size_t a8logRingPeek(const uint8_t **data);   // contiguous bytes at the tail
void a8logRingConsume(size_t n);
uint8_t a8logRingTakeLevel();                 // most severe level since the last take
uint32_t a8logRingDropped();

// From SidecarHost::begin(), on the owner task, once the Sidecar has
// answered its boot report: send LOG() text from here on if it can print
// it (SidecarCapability::SerialOut) and the route is wanted (csim:
// CSIM_LOG_ROUTE=sidecar|both).
void a8logSidecarStart(bool sidecarCanPrint);
// The link failed to take the text: back to the console for good.
void a8logSidecarStop(const char *why);
bool a8logSidecarRoute();

// Send one frame's worth if any text waits: for the hot PBI request path
// (handlePbiRequest(), the 6502 parked). IRAM, memory only.
void a8logDrainHot();
// Send everything waiting, if the caller may (the owner task, interrupts
// enabled): before an exit, after a burst of output.
void a8logFlush();
// End of run: the route's counters, if it was ever on. Cold.
void a8logReport();
#else
static inline void a8logDrainHot() {}
static inline void a8logFlush() {}
static inline void a8logReport() {}
#endif
