#pragma once

#ifdef PROFILE

#include <stdint.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "sdkconfig.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "core0_hotspots.h"

#ifndef TEST_SEC
#define TEST_SEC -1
#endif

#define PROFILE_GPIO_SAMPLE_MODE_POLLING 0
#define PROFILE_GPIO_SAMPLE_MODE_BURST 1
#define PROFILE_GPIO_SAMPLE_MODE PROFILE_GPIO_SAMPLE_MODE_BURST
#if PROFILE_GPIO_SAMPLE_MODE != PROFILE_GPIO_SAMPLE_MODE_POLLING && \
    PROFILE_GPIO_SAMPLE_MODE != PROFILE_GPIO_SAMPLE_MODE_BURST
#error "Invalid PROFILE_GPIO_SAMPLE_MODE"
#endif

#define PROFILE_GPIO_RAISE() REG_WRITE(GPIO_OUT1_W1TS_REG, 1u << (profPin - 32))
#define PROFILE_GPIO_START() do {} while (0)
#define PROFILE_GPIO_STOP() do {} while (0)
#define PROFILE_LOOP_START() do {} while (0)

#if TEST_SEC > 0
#ifndef PROFILE_GPIO_SAMPLE_CYCLES
static_assert(static_cast<uint64_t>(TEST_SEC) * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000ULL <= UINT32_MAX,
    "TEST_SEC exceeds the CCOUNT sampler's maximum interval");
#define PROFILE_GPIO_SAMPLE_CYCLES (static_cast<uint32_t>(static_cast<uint64_t>(TEST_SEC) * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000ULL))
#endif
#endif

#if (defined(PROF0) + defined(PROF1) + defined(PROF2) + defined(PROF3) + \
     defined(PROF4) + defined(PROF5) + defined(PROF6)) > 1
#error Select only one PROF0 through PROF6 point for GPIO profiling
#endif

#define PROFILE0(ticks) do {} while (0)
#define PROFILE1(ticks) do {} while (0)
#define PROFILE2(ticks) do {} while (0)
#define PROFILE3(ticks) do {} while (0)
#define PROFILE4(ticks) do {} while (0)
#define PROFILE5(ticks) do {} while (0)
#define PROFILE6(ticks) do {} while (0)

#ifdef PROF0
#undef PROFILE0
#define PROFILE0(ticks) PROFILE_GPIO_RAISE()
#endif
#ifdef PROF1
#undef PROFILE1
#define PROFILE1(ticks) PROFILE_GPIO_RAISE()
#endif
#ifdef PROF2
#undef PROFILE2
#define PROFILE2(ticks) PROFILE_GPIO_RAISE()
#endif
#ifdef PROF3
#undef PROFILE3
#define PROFILE3(ticks) PROFILE_GPIO_RAISE()
#endif
#ifdef PROF4
#undef PROFILE4
#define PROFILE4(ticks) PROFILE_GPIO_RAISE()
#endif
#ifdef PROF5
#undef PROFILE5
#define PROFILE5(ticks) PROFILE_GPIO_RAISE()
#endif
#ifdef PROF6
#undef PROFILE6
#define PROFILE6(ticks) PROFILE_GPIO_RAISE()
#endif

#define PROFILE_BMON(ticks) profilers[0].add(ticks)
#define PROFILE_MMU(ticks) profilers[0].add(ticks)
#define PROFILE_START() do {} while (0)

struct Hist2 {
    static const DRAM_ATTR int maxBucket = 1024;
    int buckets[maxBucket];
    inline IRAM_ATTR void clear() { for (int i = 0; i < maxBucket; ++i) buckets[i] = 0; }
    inline IRAM_ATTR void add(uint32_t value) { buckets[value & (maxBucket - 1)]++; }
    Hist2() { clear(); }
    int64_t count() {
        int64_t sum = 0;
        for (int i = 0; i < maxBucket; ++i) sum += buckets[i];
        return sum;
    }
};

static const DRAM_ATTR int numProfilers = 5;
extern DRAM_ATTR Hist2 profilers[numProfilers];

struct ProfileGpioSampleResult {
    uint32_t elapsedCycles;
    uint32_t captures;
    uint32_t noClockFall;
    uint32_t extSelTransitions;
    uint32_t dataTransitions;
    uint32_t addressTransitions;
    uint32_t readTransitions;
    uint32_t writeTransitions;
    uint32_t inputOffset;
};

void profileGpioInitCore1();
void profileGpioInitCore0();
CORE0_HOTSPOTS_ATTR ProfileGpioSampleResult profileGpioSampleForCycles(uint32_t cycleBudget);

#else

#define PROFILE_GPIO_START() do {} while (0)
#define PROFILE_GPIO_STOP() do {} while (0)
#define PROFILE_LOOP_START() do {} while (0)
#define PROFILE_START() do {} while (0)
#define PROFILE_BMON(ticks) do {} while (0)
#define PROFILE_MMU(ticks) do {} while (0)
#define PROFILE0(ticks) do {} while (0)
#define PROFILE1(ticks) do {} while (0)
#define PROFILE2(ticks) do {} while (0)
#define PROFILE3(ticks) do {} while (0)
#define PROFILE4(ticks) do {} while (0)
#define PROFILE5(ticks) do {} while (0)
#define PROFILE6(ticks) do {} while (0)

#endif
