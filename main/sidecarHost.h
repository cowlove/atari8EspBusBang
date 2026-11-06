#pragma once

// Host use of the Sidecar coprocessor (A8BB_SIDECAR_ENABLE): the host's end
// of the link and a startup check. Commands are sent through `sidecar`
// (Sidecar::sio(), cio(), nop(); see components/sidecar_link/sidecar.h).

#include "sdkconfig.h"

#if CONFIG_A8BB_SIDECAR_ENABLE
#include "sidecar.h"

extern Sidecar sidecar;

// Start the link (the host follows the Sidecar's clock), probe for the
// Sidecar and log the result. Call once from setup().
void sidecarHostSetup();
#endif
