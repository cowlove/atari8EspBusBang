#pragma once

#if defined(PROFILE_GPIO_SAMPLE) || defined(PROF0) || defined(PROF1) || \
    defined(PROF2) || defined(PROF3) || defined(PROF4) || \
    defined(PROF5) || defined(PROF6)
#define PROFILE_ENABLE_ARRAY
#endif

#if (defined(PROF0) + defined(PROF1) + defined(PROF2) + defined(PROF3) + \
     defined(PROF4) + defined(PROF5) + defined(PROF6)) > 1
#error Select only one PROF0 through PROF6 point
#endif

#if defined(PROFILE_GPIO_SAMPLE) && \
    (defined(PROF0) || defined(PROF1) || defined(PROF2) || defined(PROF3) || \
     defined(PROF4) || defined(PROF5) || defined(PROF6))
#define PROFILE_GPIO_PIN_OUTPUT
#endif

#ifdef PROFILE_ENABLE_ARRAY

#include <stdint.h>
#include <inttypes.h>
#include "esp_attr.h"

#ifdef PROFILE_GPIO_SAMPLE
#include "sdkconfig.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "core0_hotspots.h"

#define PROFILE_GPIO_SAMPLE_MODE_POLLING 0
#define PROFILE_GPIO_SAMPLE_MODE_BURST 1
#define PROFILE_GPIO_SAMPLE_MODE PROFILE_GPIO_SAMPLE_MODE_BURST
#if PROFILE_GPIO_SAMPLE_MODE != PROFILE_GPIO_SAMPLE_MODE_POLLING && \
    PROFILE_GPIO_SAMPLE_MODE != PROFILE_GPIO_SAMPLE_MODE_BURST
#error "Invalid PROFILE_GPIO_SAMPLE_MODE"
#endif

#if defined(RUN_SEC) && RUN_SEC > 0
#ifndef PROFILE_GPIO_SAMPLE_CYCLES
static constexpr uint64_t profileGpioRequestedCycles =
    static_cast<uint64_t>(RUN_SEC) * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000ULL;
#define PROFILE_GPIO_SAMPLE_CYCLES (static_cast<uint32_t>(profileGpioRequestedCycles > UINT32_MAX ? UINT32_MAX : profileGpioRequestedCycles))
#endif
#endif
#endif

struct Hist2 {
    static const DRAM_ATTR int maxBucket = 128;
    uint32_t buckets[maxBucket];
    static DRAM_ATTR bool binsFull;
    inline IRAM_ATTR void clear() { for (int i = 0; i < maxBucket; ++i) buckets[i] = 0; }
    inline IRAM_ATTR void add(uint32_t value) { 
        const int idx = value & (maxBucket - 1);
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

#ifdef PROFILE_GPIO_SAMPLE
#ifdef PROFILE_GPIO_PIN_OUTPUT
static const DRAM_ATTR int numProfilers = 1;
#else
static const DRAM_ATTR int numProfilers = 7;
#endif
#else
static const DRAM_ATTR int numProfilers = 1;
#endif

extern DRAM_ATTR Hist2 profilers[numProfilers];

 
struct ProfileGpioSampleResult {
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

#ifdef PROFILE_GPIO_SAMPLE
void profileGpioInitCore0();
CORE0_HOTSPOTS_ATTR ProfileGpioSampleResult profileGpioSampleForCycles(uint32_t cycleBudget);
#endif

#ifdef PROFILE_GPIO_PIN_OUTPUT
extern DRAM_ATTR uint32_t profileGpioOutMask;
#define PROFILE_GPIO_PIN_SET() dedic_gpio_cpu_ll_write_mask(profileGpioOutMask, profileGpioOutMask)
#define PROFILE_GPIO_PIN_CLEAR() dedic_gpio_cpu_ll_write_mask(profileGpioOutMask, 0)
#else
#define PROFILE_GPIO_PIN_SET() do {} while (0)
#define PROFILE_GPIO_PIN_CLEAR() do {} while (0)
#endif

#ifdef PROFILE_GPIO_SAMPLE
#define PROFILE_LOOP_START() do {} while (0)
#else
#define PROFILE_LOOP_START() uint32_t tscFall = XTHAL_GET_CCOUNT()
#endif

#define PROFILE_GPIO_START() do {} while (0)
#define PROFILE_GPIO_STOP() do {} while (0)
#define PROFILE_START() do {} while (0)
#define PROFILE_BMON(ticks) profilers[0].add(ticks)
#define PROFILE_MMU(ticks) profilers[0].add(ticks)

#ifdef PROF0
#define PROFILE0() PROFILE_GPIO_PIN_SET()
#else
#define PROFILE0() do {} while (0)
#endif
#ifdef PROF1
#define PROFILE1() PROFILE_GPIO_PIN_SET()
#else
#define PROFILE1() do {} while (0)
#endif
#ifdef PROF2
#define PROFILE2() PROFILE_GPIO_PIN_SET()
#else
#define PROFILE2() do {} while (0)
#endif
#ifdef PROF3
#define PROFILE3() PROFILE_GPIO_PIN_SET()
#else
#define PROFILE3() do {} while (0)
#endif
#ifdef PROF4
#define PROFILE4() PROFILE_GPIO_PIN_SET()
#else
#define PROFILE4() do {} while (0)
#endif
#ifdef PROF5
#define PROFILE5() PROFILE_GPIO_PIN_SET()
#else
#define PROFILE5() do {} while (0)
#endif
#ifdef PROF6
#define PROFILE6() PROFILE_GPIO_PIN_SET()
#else
#define PROFILE6() do {} while (0)
#endif

#if !defined(PROFILE_GPIO_SAMPLE)
#ifdef PROF0
#undef PROFILE0
#define PROFILE0() profilers[0].add(XTHAL_GET_CCOUNT() - tscFall)
#endif
#ifdef PROF1
#undef PROFILE1
#define PROFILE1() profilers[0].add(XTHAL_GET_CCOUNT() - tscFall)
#endif
#ifdef PROF2
#undef PROFILE2
#define PROFILE2() profilers[0].add(XTHAL_GET_CCOUNT() - tscFall)
#endif
#ifdef PROF3
#undef PROFILE3
#define PROFILE3() profilers[0].add(XTHAL_GET_CCOUNT() - tscFall)
#endif
#ifdef PROF4
#undef PROFILE4
#define PROFILE4() profilers[0].add(XTHAL_GET_CCOUNT() - tscFall)
#endif
#ifdef PROF5
#undef PROFILE5
#define PROFILE5() profilers[0].add(XTHAL_GET_CCOUNT() - tscFall)
#endif
#ifdef PROF6
#undef PROFILE6
#define PROFILE6() profilers[0].add(XTHAL_GET_CCOUNT() - tscFall)
#endif
#endif

#else

#define PROFILE_GPIO_START() do {} while (0)
#define PROFILE_GPIO_STOP() do {} while (0)
#define PROFILE_GPIO_PIN_SET() do {} while (0)
#define PROFILE_GPIO_PIN_CLEAR() do {} while (0)
#define PROFILE_LOOP_START() do {} while (0)
#define PROFILE_START() do {} while (0)
#define PROFILE_BMON(ticks) do {} while (0)
#define PROFILE_MMU(ticks) do {} while (0)
#define PROFILE0() do {} while (0)
#define PROFILE1() do {} while (0)
#define PROFILE2() do {} while (0)
#define PROFILE3() do {} while (0)
#define PROFILE4() do {} while (0)
#define PROFILE5() do {} while (0)
#define PROFILE6() do {} while (0)

#endif
