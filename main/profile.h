#pragma once
#include <stdint.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "sdkconfig.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "core0_hotspots.h"

#ifndef TEST_SEC
#define TEST_SEC -1
#endif

// Select new hw-assisted profiling
#define PROFILE_GPIO_ENABLED 1

#define PROFILE_GPIO_SAMPLE_MODE_POLLING 0
#define PROFILE_GPIO_SAMPLE_MODE_BURST 1
#define PROFILE_GPIO_SAMPLE_MODE PROFILE_GPIO_SAMPLE_MODE_POLLING
#if PROFILE_GPIO_SAMPLE_MODE != PROFILE_GPIO_SAMPLE_MODE_POLLING && \
    PROFILE_GPIO_SAMPLE_MODE != PROFILE_GPIO_SAMPLE_MODE_BURST
#error "Invalid PROFILE_GPIO_SAMPLE_MODE"
#endif

#if PROFILE_GPIO_ENABLED
#define PROFILE_GPIO_START() {}
#define PROFILE_GPIO_STOP() {}
#define PROFILE_GPIO_RAISE() dedic_gpio_cpu_ll_write_all(1u)
#define PROFILE_LOOP_START() {}
#else
#define PROFILE_GPIO_START() uint32_t tscFall = XTHAL_GET_CCOUNT()
#define PROFILE_GPIO_STOP() {}
#define PROFILE_LOOP_START() {}
#endif

#if PROFILE_GPIO_ENABLED && TEST_SEC > 0
#ifndef PROFILE_GPIO_SAMPLE_CYCLES
static_assert(static_cast<uint64_t>(TEST_SEC) * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000ULL <= UINT32_MAX,
    "TEST_SEC exceeds the CCOUNT sampler's maximum interval");
#define PROFILE_GPIO_SAMPLE_CYCLES (static_cast<uint32_t>(static_cast<uint64_t>(TEST_SEC) * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000ULL))
#endif
#endif

#if (defined(PROF1) + defined(PROF2) + defined(PROF3) + defined(PROF4) + defined(PROF5) + defined(PROF6)) > 1
#error Select only one PROF1 through PROF6 point for GPIO profiling
#endif

#define PROFILE0(a) {}
#define PROFILE1(a) {}
#define PROFILE2(a) {}
#define PROFILE3(a) {}
#define PROFILE4(a) {}
#define PROFILE5(a) {}
#define PROFILE6(a) {}

#ifdef PROF0
#undef PROFILE0
#if PROFILE_GPIO_ENABLED
#define PROFILE0(ticks) PROFILE_GPIO_RAISE()
#else
#define PROFILE0(ticks) profilers[0].add(ticks)
#endif
#define FAKE_CLOCK
#endif
#ifdef PROF1
#undef PROFILE1
#if PROFILE_GPIO_ENABLED
#define PROFILE1(ticks) PROFILE_GPIO_RAISE()
#else
#define PROFILE1(ticks) profilers[0].add(ticks)
#endif
#define FAKE_CLOCK
#endif
#ifdef PROF2
#undef PROFILE2
#if PROFILE_GPIO_ENABLED
#define PROFILE2(ticks) PROFILE_GPIO_RAISE()
#else
#define PROFILE2(ticks) profilers[0].add(ticks)
#endif
#define FAKE_CLOCK
#endif
#ifdef PROF3
#undef PROFILE3
#if PROFILE_GPIO_ENABLED
#define PROFILE3(ticks) PROFILE_GPIO_RAISE()
#else
#define PROFILE3(ticks) profilers[0].add(ticks)
#endif
#define FAKE_CLOCK
#endif
#ifdef PROF4
#undef PROFILE4
#if PROFILE_GPIO_ENABLED
#define PROFILE4(ticks) PROFILE_GPIO_RAISE()
#else
#define PROFILE4(ticks) profilers[0].add(ticks)
#endif
#define FAKE_CLOCK
#endif
#ifdef PROF6
#undef PROFILE6
#if PROFILE_GPIO_ENABLED
#define PROFILE6(ticks) PROFILE_GPIO_RAISE()
#else
#define PROFILE6(ticks) profilers[0].add(ticks)
#endif
#define FAKE_CLOCK
#endif
#ifdef PROF5
#undef PROFILE5
#if PROFILE_GPIO_ENABLED
#define PROFILE5(ticks) PROFILE_GPIO_RAISE()
#else
#define PROFILE5(ticks) profilers[0].add(ticks)
#endif
#define FAKE_CLOCK
#endif

#ifdef FAKE_CLOCK
#define PROFILE_BMON(ticks) {}
#define PROFILE_MMU(ticks) {}
#define PROFILE_START() uint32_t tscFall = XTHAL_GET_CCOUNT()
#else
#define PROFILE_BMON(ticks) profilers[0].add(ticks)
#define PROFILE_MMU(ticks) profilers[0].add(ticks)
#define PROFILE_START() {}
#endif

struct Hist2 { 
    static const DRAM_ATTR int maxBucket = 1024; // must be power of 2
    int buckets[maxBucket];
    inline IRAM_ATTR void clear() { for(int i = 0; i < maxBucket; i++) buckets[i] = 0; }
    inline IRAM_ATTR void add(uint32_t x) { buckets[x & (maxBucket - 1)]++; }
    Hist2() { clear(); }
    int64_t count() {
        int64_t sum = 0; 
        for(int i = 0; i < maxBucket; i++) sum += buckets[i];
        return sum;
    }
};

static const DRAM_ATTR int numProfilers = 1;
extern DRAM_ATTR Hist2 profilers[numProfilers];

#if PROFILE_GPIO_ENABLED
struct ProfileGpioSampleResult {
    uint32_t elapsedCycles;
    uint32_t captures;
    uint32_t validIntervals;
    uint32_t noClockFall;
    uint32_t fallWithoutProfileToggle;
    uint32_t rejectedCaptures;
    uint32_t inputOffset;
};

void profileGpioInitCore1();
void profileGpioInitCore0();
CORE0_HOTSPOTS_ATTR ProfileGpioSampleResult profileGpioSampleForCycles(uint32_t cycleBudget);
#endif
