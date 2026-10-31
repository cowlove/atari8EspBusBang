#include "profile.h"

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

DRAM_ATTR Hist2 profilers[numProfilers];
bool DRAM_ATTR Hist2::binsFull = false;

void profileClear() {
    for (int i = 0; i < numProfilers; ++i)
        profilers[i].clear();
}

#ifdef PROFILE_GPIO_SAMPLE
static constexpr int profileGpioSampleCount = 90;
static constexpr int profileGpioTestFreq = 1.78 * 1000000;
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
#ifdef PROFILE_GPIO_PIN_OUTPUT
    const int gpioArray[] = {bus.clock.pin, profPin};
    printf("profileGpioInitCore0: configuring dedic_gpio_bundle for 2 inputs\n");
#else
    const int gpioArray[] = {
        bus.clock.pin,
        profPin,
        bus.rw.pin,
        bus.extSel.pin,
        bus.data.pin,
        bus.data.pin + 1,
        bus.addr.pin,
        bus.refresh_.pin
    };
    printf("profileGpioInitCore0: configuring dedic_gpio_bundle for 8 inputs\n");
#endif
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
    gpio_matrix_out(profPin, CORE1_GPIO_OUT0_IDX, false, false);
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

    while (static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start) < cycleBudget) {
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

        const int busSamplePt = 36;
        const bool refreshHigh = profileGpioSamples[busSamplePt] & refreshMask;
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
#ifdef PROFILE_GPIO_PIN_OUTPUT
            bool profileTransitionRecorded = false;
#endif
            for (int i = fallingSample + 1; i < profileGpioSampleCount; ++i) {
                const uint8_t changedPins = profileGpioSamples[i - 1] ^ profileGpioSamples[i];
                const uint32_t delta = static_cast<uint32_t>(i - fallingSample);
#ifdef PROFILE_GPIO_PIN_OUTPUT
                if ((changedPins & profileMask) && !(profileGpioSamples[i - 1] & profileMask) &&
                    (profileGpioSamples[i] & profileMask) && !profileTransitionRecorded) {
                    profilers[0].add(delta);
                    ++result.profileTransitions;
                    ++transitionsThisCapture;
                    eventSample = i;
                    profileTransitionRecorded = true;
                }
#endif
#ifndef PROFILE_GPIO_PIN_OUTPUT
                if (changedPins & extSelMask) {
                    profilers[readHigh ? 1 : 0].add(delta);
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
                                profilers[extSelHigh ? 3 : 2].add(delta);
                            else
                                profilers[extSelHigh ? 5 : 4].add(delta);
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
                            profilers[6].add(delta);
                            ++result.addressTransitions;
                            if (readHigh) ++result.readTransitions;
                            else ++result.writeTransitions;
                            ++transitionsThisCapture;
                            eventSample = i;
                        }
                    }
                }
                if (changedPins & rwMask) {
                    profilers[7].add(delta);
                    ++transitionsThisCapture;
                    eventSample = i;
                }
#endif
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
#ifdef PROFILE_GPIO_PIN_OUTPUT
        printf("COLUMNS: profile_pin\n");
#else
        printf("COLUMNS: extSel_rd extsel_wr rd_extSel rd_normal wr_extSel wr_normal addr_all\n");
#endif
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
            printf("% 4d ", i);
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
