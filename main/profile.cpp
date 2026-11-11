#include "profile.h"
#include "fakeBus.h"

#ifdef PROFILE_ENABLE_ARRAY

#include "main.h"
#include "pinDefs.h"
#ifndef ARDUINO
#include "arduinoLite.h"
#endif

#include <inttypes.h>
#include <stdio.h>

#include "esp_err.h"
#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "rom/gpio.h"
#include "soc/gpio_sig_map.h"
#include "xtensa/core-macros.h"

/* 
 * NOTES for profiling on bench/unattached boards with bench-idf.sh:
 * 
 * The FAKE_CLOCK clock is relaxed to 1.50Mhz instead of the actual 1.78Mhz seen on hardware.  With the additional profiling 
 * instructions in core1, we typically miss bus timing when running at the true rate and miss the next clock edge, scattering 
 * sampled data.  The latency of all data points are still valid, but don't get confused by referencing against the artifically slower
 * sampled clock transitions.  Pretend the end of the clock cycle is still at 132 ticks. 
 *
 * The reset of the profiling pin is set just before the falling-clock busy wait, so it represents the completion of the loop
 * logic.  This way, the second transition of the profiling pin can measure loop completion and margin against the next
 * clock fall. 
 *
 * In FAKE_CLOCK, the artifical rw and addr transitions are scattered all over the clock period.  The profiling loop samples i
 * extSel and RW at what it thinks is a stable point to decide how to tally the bus cycle's info, but this is meaningless in FAKE_CLOCK.  
 * Profiled data is randomly scattered between read and write bins, and these bins should be considered together as merged data. 
 *
 * PROBLEM: I do not understand why extSel_ is sometimes sampled high and data categorized into the "normal" read/writes in bench
 * tests.  In bench testing every memory operation should be to address 0x180 and 0x181 and extSel should be low.  There should 
 * be no "normal" read/write profile points. 
 *
 * NOTES for profiling on real hardware:
 *
 * Since the data is categorized based on the current cycles' RW and extSel settings, the first early transition of extSel and data 
 * is from the previous cycle.  The interesting and properly categorized transitions of extSel and data are the second transition
 * where the pins are driven, and the third transition after the final falling clock when they are released by this clock cycle's 
 * code path. The first transition is a randomly-categorized imprint of the previous cycle's release.  They can be summed together
 * and used as a more accurate measure of the merged data against the recent clock fall, but they are inaccurately categorized. 
 */

DRAM_ATTR Hist2 profilers[numProfilers];

void profileClear() {
    for (int i = 0; i < numProfilers; ++i)
        profilers[i].clear();
}

#ifdef PROFILE_GPIO_SAMPLE
#ifdef FAKE_CLOCK
static constexpr int profileGpioTestFreq = fakeClockHz;   // fakeBus.h
#else
static constexpr int profileGpioTestFreq = 1.78 * 1000000;
#endif
static constexpr uint32_t profileGpioHalfCycleTicks = 240 * 1000000 / profileGpioTestFreq / 2;
static_assert(numProfilers == busProfilerChannels, "the GPIO histograms are bus_profiler's channels");
static DRAM_ATTR bool profileGpioBundleReady = false;
static DRAM_ATTR uint32_t profileGpioInOffset = 0;
static DRAM_ATTR BusProfilerState profileGpioState = {};
static const BusProfilerPins profileGpioPins = {
    bus.clock.pin, profPin, bus.rw.pin, bus.extSel.pin,
    bus.data.pin, bus.data.pin + 1, bus.addr.pin,
#ifdef HAVE_REFRESH
    bus.refresh_.pin,
#else
    -1,
#endif
};

#ifdef PROFILE_GPIO_PIN_OUTPUT
DRAM_ATTR uint32_t profileGpioOutMask = 1;
#endif

void profileGpioInitCore0() {
    profileGpioInOffset = busProfilerInitBundle(profileGpioPins);
    profileGpioBundleReady = true;
}

void profileGpioInitCore1() {
#ifdef PROFILE_GPIO_PIN_OUTPUT
    pinMode(profPin, OUTPUT | PULLUP);
    digitalWrite(profPin, 1);
    esp_rom_gpio_connect_out_signal(profPin, CORE1_GPIO_OUT0_IDX, false, false);
#endif
}

// The capture (components/bus_profiler), placed in core 0's hot-bus code.
// FAKE_CLOCK's R/W and A0 are phase-locked to its clock (4- and 8-cycle
// patterns), so captures there skip a random 0-7 clock periods first.
CORE0_HOTSPOTS_ATTR void profileGpioSampleBurstForCycles(uint32_t cycleBudget) {
    if (!profileGpioBundleReady)
        profileGpioInitCore0();
    constexpr bool skipPhases =
#ifdef FAKE_CLOCK
        true;
#else
        false;
#endif
    constexpr bool haveRefresh =
#ifdef HAVE_REFRESH
        true;
#else
        false;
#endif
    constexpr bool recordProfilePin =
#ifdef PROFILE_GPIO_PIN_OUTPUT
        true;
#else
        false;
#endif
    busProfilerCaptureForCycles<skipPhases, haveRefresh, recordProfilePin, profileGpioHalfCycleTicks>(
        profileGpioState, profilers, profileGpioInOffset, cycleBudget);
}
#endif

void profilePrint(bool histogram, int ramReads) {
    uint64_t totalEvents = 0;
    printf("Total samples %" PRIu64 " implies %.2f sec sampling. Total reads %d\n",
        totalEvents, 1.0 * totalEvents / 1.8 / 1000000, ramReads);

    if (histogram)
        busProfilerPrintHistograms(profilers, numProfilers);

#ifdef PROFILE_GPIO_SAMPLE
    constexpr bool profilePinOutput =
#ifdef PROFILE_GPIO_PIN_OUTPUT
        true;
#else
        false;
#endif
    busProfilerPrintCaptures(profileGpioState, profileGpioPins, profilePinOutput);
#endif
}

#endif
