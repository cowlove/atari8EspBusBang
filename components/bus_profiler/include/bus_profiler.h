#pragma once
// bus_profiler: burst capture of the Atari bus pins against the phi2 clock,
// used by the host app (main/profile.cpp, PROFILE_MODE=GPIO/PIN). Planned:
// the Sidecar app, sampling on its free core (docs/sidecar_wiring.pdf has
// its input pins).
//
// One capture waits for a phi2 fall, then reads the eight pins of a
// dedicated-GPIO input bundle back to back (~2 CPU cycles per read, at
// 240 MHz) from just before the fall to past the next one, and adds each
// pin's edges to histograms by their distance from the fall. The bundle's
// bit order is fixed (see BusProfilerPins): the classifier depends on it.
//
// The capture loop is always_inline, so each app wraps it in its own
// function with its own placement (the host keeps it in its core 0 hot-bus
// section). A dedicated-GPIO bundle belongs to the core that created it:
// create it on the core that runs the capture.

#include <stdint.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "xtensa/core-macros.h"

// The bundle's pins, in bundle bit order. refresh_ may be -1 (none).
struct BusProfilerPins {
    int clock;      // bit 0: phi2
    int profile;    // bit 1: the host's profile pin (GPIO48)
    int rw;         // bit 2: R/W
    int extSel;     // bit 3: extSel_
    int d0;         // bit 4: D0
    int d1;         // bit 5: D1
    int a0;         // bit 6: A0
    int refresh;    // bit 7: refresh_ (active low), or -1
};

struct Hist2 {
    static const DRAM_ATTR int maxBucket = 192;
    uint32_t buckets[maxBucket];
    static DRAM_ATTR bool binsFull;
    inline IRAM_ATTR void clear() { for (int i = 0; i < maxBucket; ++i) buckets[i] = 0; }
    inline IRAM_ATTR void add(uint32_t value) {
        const int idx = value < maxBucket ? value : maxBucket - 1;  // late values pile up in the last bucket
        buckets[idx]++;
        if (buckets[idx] > 0xf0000000) binsFull = true;
    }
    Hist2() { clear(); }
    int64_t count() {
        int64_t sum = 0;
        for (int i = 0; i < maxBucket; ++i) sum += buckets[i];
        return sum;
    }
};

// Histogram channels: 0 profile-pin edges; 1/2 extSel_ edges on write/read
// cycles; 3/4 D0-D1 edges on reads with extSel_ low/high; 5/6 the same on
// writes; 7 A0 edges; 8 R/W edges; 9 clock edges. R/W and extSel_ are
// judged at read 44 (~88 ticks), mid-cycle.
static constexpr int busProfilerChannels = 10;

struct BusProfilerResult {
    uint32_t elapsedCycles;
    uint32_t captures;
    uint32_t noClockFall;
    uint32_t profileTransitions;
    uint32_t extSelTransitions;
    uint32_t dataTransitions;
    uint32_t addressTransitions;
    uint32_t readTransitions;
    uint32_t writeTransitions;
    uint32_t inputOffset;
};

// About 2 CPU cycles per read: from ~10 cycles before the clock fall to
// past the next fall, even at the bench's slower fake clock (168 cycles).
static constexpr int busProfilerReads = 110;

struct BusProfilerState {
    BusProfilerResult result;
    uint8_t samples[busProfilerReads];
    uint8_t validSamples[busProfilerReads];
    bool hasValidSamples;
    uint32_t validFallSample;
    uint32_t validEventSample;
};

// Creates the input bundle for pins on the calling core; returns its bit
// offset in dedic_gpio_cpu_ll_read_in().
uint32_t busProfilerInitBundle(const BusProfilerPins &pins);

// Capture for cycleBudget CPU cycles into hist[0..busProfilerChannels-1].
//   SkipPhases: skip a pseudo-random 0-7 clock periods before each capture,
//     so a fixed capture-to-capture time can't lock onto a periodic bus
//     pattern (the bench fake clock's R/W and A0, or a 6502 loop).
//   HaveRefresh: bit 7 is refresh_, and data edges of refresh cycles are
//     dropped (their odd timing scatters the data histograms).
//   RecordProfilePin: count profile-pin edges (channel 0).
//   HalfCycleTicks: half a bus clock period in CPU cycles, to start reading
//     ~10 cycles before the fall (an asm repeat count: a constant).
//   BoundedWaits: give up waiting for a clock edge at the end of the budget
//     (the clock can stop: the Sidecar's sampler, when the main board is
//     reset or being flashed). Off, the waits are unbounded and the code is
//     what the host's hot-bus capture has always run.
template <bool SkipPhases, bool HaveRefresh, bool RecordProfilePin, uint32_t HalfCycleTicks,
          bool BoundedWaits = false>
static inline __attribute__((always_inline)) void busProfilerCaptureForCycles(
        BusProfilerState &state, Hist2 *hist, uint32_t inputOffset, uint32_t cycleBudget) {
    BusProfilerResult result = {};
    const uint32_t start = XTHAL_GET_CCOUNT();
    auto budgetSpent = [&]() __attribute__((always_inline)) {
        return static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start) >= cycleBudget;
    };
    (void)budgetSpent;
    const uint32_t clockMask = 1u << inputOffset;
    const uint32_t profileMask = 1u << (inputOffset + 1);
    result.inputOffset = inputOffset;
    const uint32_t rwMask = 1u << (inputOffset + 2);
    const uint32_t extSelMask = 1u << (inputOffset + 3);
    const uint32_t dataMask = 0x3u << (inputOffset + 4);
    const uint32_t addressMask = 0x1u << (inputOffset + 6);
    const uint32_t refreshMask = 0x1u << (inputOffset + 7);

    uint32_t skipLfsr = 0xace1u;
    (void)skipLfsr;
    while (static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start) < cycleBudget) {
        if (SkipPhases) {
            skipLfsr ^= skipLfsr << 13;
            skipLfsr ^= skipLfsr >> 17;
            skipLfsr ^= skipLfsr << 5;
            for (uint32_t skip = skipLfsr & 7; skip > 0; --skip) {
                while ((dedic_gpio_cpu_ll_read_in() & clockMask) && !(BoundedWaits && budgetSpent())) {}
                while (!(dedic_gpio_cpu_ll_read_in() & clockMask) && !(BoundedWaits && budgetSpent())) {}
            }
        }
        while ((dedic_gpio_cpu_ll_read_in() & clockMask) && !(BoundedWaits && budgetSpent())) {}
        while (!(dedic_gpio_cpu_ll_read_in() & clockMask) && !(BoundedWaits && budgetSpent())) {}
        if (BoundedWaits && budgetSpent()) {
            ++result.noClockFall;   // the clock stopped
            break;
        }

        constexpr uint32_t prefallNops = HalfCycleTicks > 10 ? HalfCycleTicks - 10 : 0;
        asm volatile(
            ".rept %0\n\t"
            "nop.n\n\t"
            ".endr\n\t"
            :
            : "i"(prefallNops)
            : "memory");

        register uint8_t *captureBase asm("a9") = state.samples;
        asm volatile(
            ".set .Lprofile_sample_offset, 0\n\t"
            ".rept %1\n\t"
            "ee.get_gpio_in a8\n\t"
            "s8i a8, %0, .Lprofile_sample_offset\n\t"
            ".set .Lprofile_sample_offset, .Lprofile_sample_offset + 1\n\t"
            ".endr\n\t"
            :
            : "r"(captureBase), "i"(busProfilerReads)
            : "a8", "memory");

        const int busSamplePt = 44;
        bool refreshHigh = true;
        if (HaveRefresh)
            refreshHigh = state.samples[busSamplePt] & refreshMask;
        else
            (void)refreshMask;
        const bool readHigh = state.samples[busSamplePt] & rwMask;
        const bool extSelHigh = state.samples[busSamplePt] & extSelMask;
        ++result.captures;

        int fallingSample = -1;
        for (int i = 1; i < busProfilerReads; ++i) {
            if ((state.samples[i - 1] & clockMask) && !(state.samples[i] & clockMask)) {
                fallingSample = i;
                break;
            }
        }

        int eventSample = -1;
        uint32_t transitionsThisCapture = 0;
        if (fallingSample >= 0) {
            for (int i = fallingSample + 1; i < busProfilerReads; ++i) {
                const uint8_t changedPins = state.samples[i - 1] ^ state.samples[i];
                const uint32_t delta = static_cast<uint32_t>(i - fallingSample);
                if (RecordProfilePin && (changedPins & profileMask)) {
                    hist[0].add(delta);
                    ++result.profileTransitions;
                    ++transitionsThisCapture;
                    eventSample = i;
                }
                if (changedPins & extSelMask) {
                    hist[readHigh ? 2 : 1].add(delta);
                    ++result.extSelTransitions;
                    ++transitionsThisCapture;
                    eventSample = i;
                }
                const uint8_t changedDataPins = changedPins & dataMask;
                const uint8_t changedAddressPins = changedPins & addressMask;
                if (changedDataPins && refreshHigh) {
                    for (int dataBit = 0; dataBit < 2; ++dataBit) {
                        if (changedDataPins & (1u << (inputOffset + 4 + dataBit))) {
                            if (readHigh)
                                hist[extSelHigh ? 4 : 3].add(delta);
                            else
                                hist[extSelHigh ? 6 : 5].add(delta);
                            ++result.dataTransitions;
                            if (readHigh) ++result.readTransitions;
                            else ++result.writeTransitions;
                            ++transitionsThisCapture;
                            eventSample = i;
                        }
                    }
                }
                if (changedAddressPins) {
                    for (int addressBit = 0; addressBit < 1; ++addressBit) {
                        if (changedAddressPins & (1u << (inputOffset + 6 + addressBit))) {
                            hist[7].add(delta);
                            ++result.addressTransitions;
                            if (readHigh) ++result.readTransitions;
                            else ++result.writeTransitions;
                            ++transitionsThisCapture;
                            eventSample = i;
                        }
                    }
                }
                if (changedPins & rwMask) {
                    hist[8].add(delta);
                    ++transitionsThisCapture;
                    eventSample = i;
                }
                if (changedPins & clockMask) {
                    hist[9].add(delta);
                    ++transitionsThisCapture;
                    eventSample = i;
                }
            }
        }

        if (fallingSample >= 0) {
            if (transitionsThisCapture > 0) {
                for (int i = 0; i < busProfilerReads; ++i)
                    state.validSamples[i] = state.samples[i];
                state.validFallSample = static_cast<uint32_t>(fallingSample);
                state.validEventSample = static_cast<uint32_t>(eventSample);
                state.hasValidSamples = true;
            }
        } else {
            ++result.noClockFall;
        }
    }

    result.elapsedCycles = static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start);
    state.result = result;
}

// The histogram table (" HIST" lines, parsed by tools/bench-idf.sh and the
// analysis scripts) and the per-channel ranges.
void busProfilerPrintHistograms(const Hist2 *hist, int channels);
// The capture summary, the bundle's pin legend and the last capture with
// an edge. profilePinOutput: the profile pin is driven (PIN-style builds).
void busProfilerPrintCaptures(const BusProfilerState &state, const BusProfilerPins &pins,
                              bool profilePinOutput);
