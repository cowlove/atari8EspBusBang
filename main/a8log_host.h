#pragma once
// The host app's LOG() plumbing with the Sidecar link (main/a8log.cpp;
// CONFIG_A8BB_LOG_VIA_SIDECAR). LOG() itself is components/a8log; the
// sending is SidecarHost::log()/logHot().
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#if defined(CONFIG_A8BB_LOG_VIA_SIDECAR) && CONFIG_A8BB_LOG_VIA_SIDECAR
// From SidecarHost::begin(), on core 0's task, once the Sidecar has
// answered its boot report: send LOG() text from here on if it can print
// it (SidecarCapability::SerialOut) and the route is wanted (csim:
// CSIM_LOG_ROUTE=sidecar|both).
void a8logSidecarStart(bool sidecarCanPrint);
// The link failed to take the text: back to the console for good. Hot-safe.
void a8logSidecarStop(const char *why);
bool a8logSidecarRoute();

// Send a partial line still in SidecarHost's line buffer (the end of a run).
void a8logFlush();
// End of run: the route's counters, if it was ever used. Cold.
void a8logReport();
#else
static inline void a8logFlush() {}
static inline void a8logReport() {}
#endif
