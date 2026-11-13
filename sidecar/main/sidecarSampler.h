#pragma once

// The Sidecar's bus sampler: burst captures of the main board's bus pins
// against phi2 (components/bus_profiler), on the Sidecar's free core 0
// while the app's service loop runs on core 1. Pins: docs/sidecar_wiring.pdf
// (hardware/tools/sidecar_wiring.py, WIRES, the profiler rows).

// Start the sampler task on core 0. Call once.
void sidecarSamplerBegin();

// Ask for the histograms and the last capture to be printed (and cleared)
// at the end of the current chunk. Safe from any task.
void sidecarSamplerRequestReport();
