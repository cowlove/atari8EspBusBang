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
bool DRAM_ATTR Hist2::binsFull = false;

void profileClear() {
    for (int i = 0; i < numProfilers; ++i)
        profilers[i].clear();
}

#ifdef PROFILE_GPIO_SAMPLE
// About 2 CPU cycles per sample: from ~10 cycles before the clock fall
// to past the next fall, even at FAKE_CLOCK's slower clock (168 cycles).
static constexpr int profileGpioSampleCount = 110;
#ifdef FAKE_CLOCK
static constexpr int profileGpioTestFreq = fakeClockHz;   // fakeBus.h
#else
static constexpr int profileGpioTestFreq = 1.78 * 1000000;
#endif
static constexpr uint32_t profileGpioHalfCycleTicks = 240 * 1000000 / profileGpioTestFreq / 2;
static DRAM_ATTR dedic_gpio_bundle_handle_t profileGpioInBundle = nullptr;
static DRAM_ATTR uint32_t profileGpioInOffset = 0;
static DRAM_ATTR ProfileGpioSampleResult profileGpioSampleResult = {};
static DRAM_ATTR uint8_t profileGpioSamples[profileGpioSampleCount] = {};
static DRAM_ATTR uint8_t profileGpioValidSamples[profileGpioSampleCount] = {};
static DRAM_ATTR bool profileGpioHasValidSamples = false;
static DRAM_ATTR uint32_t profileGpioValidFallSample = 0;
static DRAM_ATTR uint32_t profileGpioValidEventSample = 0;

#ifdef PROFILE_GPIO_PIN_OUTPUT
DRAM_ATTR uint32_t profileGpioOutMask = 1;
#endif

void profileGpioInitCore0() {
    // Inputs only: the bundle adds GPIO-matrix input routes and leaves
    // whatever drives a pin alone (in Sidecar builds, GPIO48 is the link's
    // BCLK and GPIO21 its RX; i2slink keeps BCLK's input enabled).
    const int gpioArray[] = {
        bus.clock.pin,
        profPin,
        bus.rw.pin,
        bus.extSel.pin,
        bus.data.pin,
        bus.data.pin + 1,
        bus.addr.pin,
#ifdef HAVE_REFRESH
        bus.refresh_.pin
#endif
    };
    printf("profileGpioInitCore0: configuring dedic_gpio_bundle for %d inputs\n",
           (int)(sizeof(gpioArray) / sizeof(gpioArray[0])));
    const dedic_gpio_bundle_config_t config = {
        .gpio_array = gpioArray,
        .array_size = sizeof(gpioArray) / sizeof(gpioArray[0]),
        .flags = {.in_en = 1},
    };
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&config, &profileGpioInBundle));
    ESP_ERROR_CHECK(dedic_gpio_get_in_offset(profileGpioInBundle, &profileGpioInOffset));
}

void profileGpioInitCore1() {
#ifdef PROFILE_GPIO_PIN_OUTPUT
    pinMode(profPin, OUTPUT | PULLUP);
    digitalWrite(profPin, 1);
    esp_rom_gpio_connect_out_signal(profPin, CORE1_GPIO_OUT0_IDX, false, false);
#endif
}

CORE0_HOTSPOTS_ATTR void profileGpioSampleBurstForCycles(uint32_t cycleBudget) {
    if (profileGpioInBundle == nullptr)
        profileGpioInitCore0();

    ProfileGpioSampleResult result = {};
    const uint32_t start = XTHAL_GET_CCOUNT();
    const uint32_t clockMask = 1u << profileGpioInOffset;
    const uint32_t profileMask = 1u << (profileGpioInOffset + 1);
    result.inputOffset = profileGpioInOffset;
    const uint32_t rwMask = 1u << (profileGpioInOffset + 2);
    const uint32_t extSelMask = 1u << (profileGpioInOffset + 3);
    const uint32_t dataMask = 0x3u << (profileGpioInOffset + 4);
    const uint32_t addressMask = 0x1u << (profileGpioInOffset + 6);
    const uint32_t refreshMask = 0x1u << (profileGpioInOffset + 7);

#ifdef FAKE_CLOCK
    // FAKE_CLOCK's R/W and A0 are phase-locked to its clock (4- and 8-cycle
    // patterns), and this loop takes a fixed time per capture, so captures
    // would keep landing on the same R/W/A0 phase. Skip a pseudo-random
    // 0-7 clock periods first so every phase gets sampled.
    uint32_t skipLfsr = 0xace1u;
#endif
    while (static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start) < cycleBudget) {
#ifdef FAKE_CLOCK
        skipLfsr ^= skipLfsr << 13;
        skipLfsr ^= skipLfsr >> 17;
        skipLfsr ^= skipLfsr << 5;
        for (uint32_t skip = skipLfsr & 7; skip > 0; --skip) {
            while (dedic_gpio_cpu_ll_read_in() & clockMask) {}
            while (!(dedic_gpio_cpu_ll_read_in() & clockMask)) {}
        }
#endif
        while (dedic_gpio_cpu_ll_read_in() & clockMask) {}
        while (!(dedic_gpio_cpu_ll_read_in() & clockMask)) {}

        const uint32_t prefallNops = profileGpioHalfCycleTicks > 10 ? profileGpioHalfCycleTicks - 10 : 0;
        asm volatile(
            ".rept %0\n\t"
            "nop.n\n\t"
            ".endr\n\t"
            :
            : "i"(prefallNops)
            : "memory");

        register uint8_t *captureBase asm("a9") = profileGpioSamples;
        asm volatile(
            ".set .Lprofile_sample_offset, 0\n\t"
            ".rept %1\n\t"
            "ee.get_gpio_in a8\n\t"
            "s8i a8, %0, .Lprofile_sample_offset\n\t"
            ".set .Lprofile_sample_offset, .Lprofile_sample_offset + 1\n\t"
            ".endr\n\t"
            :
            : "r"(captureBase), "i"(profileGpioSampleCount)
            : "a8", "memory");

        const int busSamplePt = 44;
#ifdef HAVE_REFRESH
        const bool refreshHigh = profileGpioSamples[busSamplePt] & refreshMask;
#else
        // No refresh_ input (Sidecar builds), so ANTIC's RAM refresh
        // cycles can't be discarded: their odd timing scatters the data
        // and address histograms. The extSel ones stay clean.
        const bool refreshHigh = true;
        (void)refreshMask;
#endif
        const bool readHigh = profileGpioSamples[busSamplePt] & rwMask;
        const bool extSelHigh = profileGpioSamples[busSamplePt] & extSelMask;
        ++result.captures;

        int fallingSample = -1;
        for (int i = 1; i < profileGpioSampleCount; ++i) {
            if ((profileGpioSamples[i - 1] & clockMask) && !(profileGpioSamples[i] & clockMask)) {
                fallingSample = i;
                break;
            }
        }

        int eventSample = -1;
        uint32_t transitionsThisCapture = 0;
        if (fallingSample >= 0) {
            for (int i = fallingSample + 1; i < profileGpioSampleCount; ++i) {
                const uint8_t changedPins = profileGpioSamples[i - 1] ^ profileGpioSamples[i];
                const uint32_t delta = static_cast<uint32_t>(i - fallingSample);
#ifdef PROFILE_GPIO_PIN_OUTPUT
                if ((changedPins & profileMask)/*  && (profileGpioSamples[i] & profileMask) && !profileTransitionRecorded*/) {
                    profilers[0].add(delta);
                    ++result.profileTransitions;
                    ++transitionsThisCapture;
                    eventSample = i;
                }
#endif
                if (changedPins & extSelMask) {
                    profilers[readHigh ? 2 : 1].add(delta);
                    ++result.extSelTransitions;
                    ++transitionsThisCapture;
                    eventSample = i;
                }
                const uint8_t changedDataPins = changedPins & dataMask;
                const uint8_t changedAddressPins = changedPins & addressMask;
                if (changedDataPins && refreshHigh) {
                    for (int dataBit = 0; dataBit < 2; ++dataBit) {
                        if (changedDataPins & (1u << (profileGpioInOffset + 4 + dataBit))) {
                            if (readHigh)
                                profilers[extSelHigh ? 4 : 3].add(delta);
                            else
                                profilers[extSelHigh ? 6 : 5].add(delta);
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
                        if (changedAddressPins & (1u << (profileGpioInOffset + 6 + addressBit))) {
                            profilers[7].add(delta);
                            ++result.addressTransitions;
                            if (readHigh) ++result.readTransitions;
                            else ++result.writeTransitions;
                            ++transitionsThisCapture;
                            eventSample = i;
                        }
                    }
                }
                if (changedPins & rwMask) {
                    profilers[8].add(delta);
                    ++transitionsThisCapture;
                    eventSample = i;
                }
                if (changedPins & clockMask) {
                    profilers[9].add(delta);
                    ++transitionsThisCapture;
                    eventSample = i;
                }
            }
        }

        if (fallingSample >= 0) {
            if (transitionsThisCapture > 0) {
                for (int i = 0; i < profileGpioSampleCount; ++i)
                    profileGpioValidSamples[i] = profileGpioSamples[i];
                profileGpioValidFallSample = static_cast<uint32_t>(fallingSample);
                profileGpioValidEventSample = static_cast<uint32_t>(eventSample);
                profileGpioHasValidSamples = true;
            }
        } else {
            ++result.noClockFall;
        }
    }

    result.elapsedCycles = static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start);
    profileGpioSampleResult = result;
}
#endif

void profilePrint(bool histogram, int ramReads) {
    uint64_t totalEvents = 0;
    printf("Total samples %" PRIu64 " implies %.2f sec sampling. Total reads %d\n",
        totalEvents, 1.0 * totalEvents / 1.8 / 1000000, ramReads);

    if (histogram) {
        printf("ticks   profile_pin   extSel_rd    extsel_wr    rd_extSel     rd_normal    wr_extSel    wr_normal    addr_all        rw_          clock\n");
        int first = profilers[0].maxBucket, last = 0;
        for (int c = 0; c < numProfilers; ++c) {
            for (int i = 1; i < profilers[c].maxBucket; ++i) {
                if (profilers[c].buckets[i] > 0 && i > last) last = i;
            }
            for (int i = profilers[c].maxBucket - 1; i > 0; --i) {
                if (profilers[c].buckets[i] > 0 && i < first) first = i;
            }
        }
        for (int i = first; i <= last; ++i) {
            printf("% 4d ", i * 2 /*i is burst sample iteration counts, about 2 cycles*/);
            for (int c = 0; c < numProfilers; ++c)
                printf("% 12d ", static_cast<int>(profilers[c].buckets[i]));
            printf(" HIST\n");
        }

        for (int c = 0; c < numProfilers; ++c) {
            first = last = 0;
            int total = 0;
            for (int i = 1; i < profilers[c].maxBucket; ++i) {
                if (profilers[c].buckets[i] > 0) last = i;
                total += profilers[c].buckets[i];
            }
            for (int i = profilers[c].maxBucket - 1; i > 0; --i) {
                if (profilers[c].buckets[i] > 0) first = i;
            }
            yield();
            printf("channel %d: range %3d -%3d, jitter %3d, total %d  HIST\n", c, first, last, last - first, total);
        }
        uint64_t histogramSamples = 0;
        for (int i = 0; i < profilers[0].maxBucket; ++i)
            histogramSamples += profilers[0].buckets[i];
        printf("Total samples %" PRIu64 " implies %.2f sec sampling\n", histogramSamples,
            1.0 * histogramSamples / 1.8 / 1000000);
    }

#ifdef PROFILE_GPIO_SAMPLE
    printf("GPIO sampler: offset %" PRIu32 ", elapsed %" PRIu32 " cycles, %" PRIu32 " captures, %" PRIu32 " without clock fall, profile edges %" PRIu32 "\n",
        profileGpioSampleResult.inputOffset, profileGpioSampleResult.elapsedCycles,
        profileGpioSampleResult.captures, profileGpioSampleResult.noClockFall,
        profileGpioSampleResult.profileTransitions);
    printf("Input bundle bits: 0=clock GPIO%d, 1=profile GPIO%d\n", bus.clock.pin, profPin);
#ifndef PROFILE_GPIO_PIN_OUTPUT
    printf("  2=R/W GPIO%d, 3=EXTSEL GPIO%d, 4=D0 GPIO%d, 5=D1 GPIO%d, 6=A0 GPIO%d, 7=A1 GPIO%d\n",
        bus.rw.pin, bus.extSel.pin, bus.data.pin, bus.data.pin + 1, bus.addr.pin, bus.addr.pin + 1);
    printf("Histogram channels: 0=profile pin; 1=all EXTSEL edges; 2/3=D0-D1 edges with R/W low/high; 4/5=A0-A1 edges with R/W low/high\n");
#else
    printf("Histogram channels: 0=first profile-pin rising edge after clock fall\n");
#endif
    if (profileGpioHasValidSamples) {
        printf("Last GPIO snapshot with an edge (clock fall=%" PRIu32 ", last edge sample=%" PRIu32 "):",
            profileGpioValidFallSample, profileGpioValidEventSample);
        for (int i = 0; i < profileGpioSampleCount; ++i) {
            if (i % 15 == 0) printf("\n%02d:", i);
            printf(" %02" PRIx8, profileGpioValidSamples[i]);
        }
    } else {
        printf("No qualified GPIO snapshot; final burst samples:");
        for (int i = 0; i < profileGpioSampleCount; ++i) {
            if (i % 15 == 0) printf("\n%02d:", i);
            printf(" %02" PRIx8, profileGpioSamples[i]);
        }
    }
    printf("\n");
#endif
}

#endif
