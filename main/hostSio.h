#pragma once
// The host's disk drives D1: through D8: (defined in main.cpp).
#include "esp_attr.h"
#include "sio.h"

extern DRAM_ATTR Sio sio;

// Mount the host's drives from their diskSpecs (config.diskSpec). A diskSpec
// that starts with "sidecar!" is mounted on the Sidecar instead, minus the
// prefix (e.g. "sidecar!smb://miner6.local/pub"); the rest go to sio.mount().
// Call after sidecarHost.begin(), so the link is up.
void mountDiskSpecs(const std::string (&diskSpecs)[Sio::kDrives]);
