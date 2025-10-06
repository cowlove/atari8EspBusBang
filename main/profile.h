#pragma once
#include <stdint.h>
#include <inttypes.h>
#include "esp_attr.h"

#define PROFILE0(a) {}
#define PROFILE1(a) {}
#define PROFILE2(a) {}
#define PROFILE3(a) {}
#define PROFILE4(a) {}
#define PROFILE5(a) {}

#ifdef PROF0
#undef PROFILE0
#define PROFILE0(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROF1
#undef PROFILE1
#define PROFILE1(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROF2
#undef PROFILE2
#define PROFILE2(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROF3
#undef PROFILE3
#define PROFILE3(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROF4
#undef PROFILE4
#define PROFILE4(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif
#define PROFILE_START() uint32_t tscFall = XTHAL_GET_CCOUNT()

#ifdef FAKE_CLOCK
#define PROFILE_BMON(ticks) {}
#define PROFILE_MMU(ticks) {}
#else
#define PROFILE_BMON(ticks) profilers[1].add(ticks)
#define PROFILE_MMU(ticks) profilers[0].add(ticks)
#endif

struct Hist2 { 
    static const DRAM_ATTR int maxBucket = 512; // must be power of 2
    int buckets[maxBucket];
    inline void clear() { for(int i = 0; i < maxBucket; i++) buckets[i] = 0; }
    inline void add(uint32_t x) { buckets[x & (maxBucket - 1)]++; }
    Hist2() { clear(); }
    int64_t count() {
        int64_t sum = 0; 
        for(int i = 0; i < maxBucket; i++) sum += buckets[i];
        return sum;
    }
};

static const DRAM_ATTR int numProfilers = 2;
extern DRAM_ATTR Hist2 profilers[numProfilers];

