#pragma once

// The Sidecar's bus sampler: burst captures of the main board's bus pins
// against phi2 (components/bus_profiler), on the Sidecar's free core 0
// while the app's service loop runs on core 1. Pins: docs/sidecar_wiring.pdf
// (hardware/tools/sidecar_wiring.py, WIRES, the profiler rows).

#include <cstdint>

// Start the sampler task on core 0. Call once.
void sidecarSamplerBegin();

// Ask for the histograms and the last capture to be printed (and cleared)
// at the end of the current chunk. Safe from any task.
void sidecarSamplerRequestReport();

// While quiet, the sampler prints nothing (its reports and pin surveys wait):
// a console file transfer owns the console's output (sidecarApp.cpp).
void sidecarSamplerQuiet(bool quiet);

// Sample periods (main's SystemControl, or the console's "sample"): Start a
// period of category (ending the one before, which is reported), Stop (the
// period ends, sampling pauses), Report (the period so far). Until the first
// control the sampler runs, category 0. Each ended period is added to its
// category's totals (categories 0-15; histograms, captures, time), kept
// until cleared: Totals prints them (category, or 255 all), ClearTotals
// clears them (likewise). Returns the periods reported so far. Safe from
// any task.
enum class SidecarSamplerAction { Stop, Start, Report, Totals, ClearTotals };
uint32_t sidecarSamplerControl(SidecarSamplerAction action, uint8_t category);
// Console: the sampler's state.
void sidecarSamplerStatus();

// A timeline of the bus for `ms` from now (main's safe reboot: what the
// 6502 does through main's reset): per `bucketMs` (at most 400 buckets),
// phi2 falls, write cycles and extSel_-low cycles in the time sampled
// (2 ms chunks with interrupts off, the scheduler between: about half the
// time at 25 ms buckets, one chunk per bucket below 10 ms), and when in the
// bucket its first write came (a parked 6502 writes nothing: the first
// write after main's restart is when it left), printed as REBOOT_TIMELINE
// lines when it ends. Taken by the sampler between captures: false if it
// isn't sampling a clock now. Mark(): a moment to show in it (the EN reset).
bool sidecarSamplerTimeline(uint32_t ms, uint32_t bucketMs = 25);
void sidecarSamplerTimelineMark(const char *label = "EN reset");   // up to 4 per timeline; label: a literal
