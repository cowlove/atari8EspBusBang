#pragma once
// The host's disk drives D1: through D8: (defined in main.cpp).
#include "esp_attr.h"
#include "core0_hotspots.h"
#include "sio.h"

extern DRAM_ATTR Sio sio;

// Mount the host's drives from their diskSpecs (config.diskSpec). A diskSpec
// that starts with "sidecar!" is mounted on the Sidecar instead, minus the
// prefix (e.g. "sidecar!smb://miner6.local/pub"); the rest go to sio.mount().
// Call after sidecarHost.begin(), so the link is up.
void mountDiskSpecs(const std::string (&diskSpecs)[Sio::kDrives]);

// The idle disk swap (config.diskIdleSwap*): the utility loop sets
// diskRotatePending when the disks have been idle long enough;
// handlePbiRequest() runs rotateDrives() first thing in the next request
// (where the Sidecar's round trip may wait, and before the next disk SIO).
// rotateDrives(): the Sidecar's drives and its mounted-unit bits
// (sidecarHost.rotateDrives()), then main's (sio.rotate()); nothing if the
// Sidecar didn't, so the two stay one set of drives.
extern DRAM_ATTR bool diskRotatePending;
CORE0_HOTSPOTS_ATTR void rotateDrives();
