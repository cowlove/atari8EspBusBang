#pragma once
// FAKE_CLOCK bench bus: a synthetic 6502 clock, R/W and A0 driven by LEDC
// on an unattached board, so core 1's loop and the profiler can run with
// no Atari. See fakeBus.cpp.
#include <stdint.h>

// One fake clock period is fakeClockApbTicks of the 80 MHz APB clock,
// ~1.43 MHz (relaxed from the real 1.78, see profile.cpp). Every fake bus
// signal runs off the same integer divider of it, so they stay in phase.
static constexpr uint32_t fakeClockApbHz = 80 * 1000000;
static constexpr uint32_t fakeClockDiv = 7;                     // APB ticks per timer count
static constexpr uint32_t fakeClockCounts = 8;                  // timer counts per clock period
static constexpr uint32_t fakeClockApbTicks = fakeClockDiv * fakeClockCounts; // 56
static constexpr uint32_t fakeClockHz = fakeClockApbHz / fakeClockApbTicks;

// Every fake bus cycle addresses this page-1 byte or the next one (A0).
static constexpr uint16_t fakeBusTestAddress = 0x0180;

#ifdef FAKE_CLOCK
// Pin setup and the phase-locked clock/R/W/A0 signals. Call from setup()
// where the bus pins are configured.
void setupFakeBusInjection();
// Map the test page for the bench (writes to dummy RAM, with extSel_).
// Call after mmuInit().
void setupFakeBusMmu();
#endif
