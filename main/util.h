#pragma once 
#include <inttypes.h>
#include "esp_attr.h"
#include "xtensa/core-macros.h"
#include "core0_hotspots.h"
#include <cstdio>
#pragma GCC diagnostic ignored "-Wunused-function"

static IRAM_ATTR void screenMemToAscii(char *buf, int buflen, uint8_t c, bool inverseVideo = true) {
    bool inv = false;
    if (c & 0x80) {
        c -= 0x80;
        inv = true;
    };
    if (c < 64) c += 32;
    else if (c < 96) c -= 64;
    if (inv && inverseVideo)
        snprintf(buf, buflen, DRAM_STR("\033[7m%c\033[0m"), c);
    else 
        snprintf(buf, buflen, DRAM_STR("%c"), c);
}

IRAM_ATTR void putKeys(const char *s, int len);
IRAM_ATTR uint8_t *checkRangeMapped(uint16_t start, uint16_t len);
void wifiRun(); 

// eg AsmNops<10>::generate() // insert 10 asm("nop;");
#ifdef CSIM
template< unsigned N > struct AsmNops{
  static inline void generate() {}
};
template<> struct AsmNops<0>{ static inline void generate() {} };

#define loopNezDelay(cycles) ((void)0)
#ifdef CSIM_BUS_CYCLE
// Bus-cycle mode: core 0's waits for the 6502 run idle bus cycles
// (csim/a800/csim_stubs.cpp), so a halt_ release core 0 makes during the
// wait (resume6502()) reaches the pin, as core 1 would apply it on hardware.
extern "C" void csim_busy_wait_6502_ticks(uint32_t ticks);
static CORE0_HOTSPOTS_ATTR void busyWait6502Ticks(uint32_t ticks) { csim_busy_wait_6502_ticks(ticks); }
#else
static CORE0_HOTSPOTS_ATTR void busyWait6502Ticks(uint32_t ticks) { (void)ticks; }
#endif
static inline void busywait(float sec) { (void)sec; }
#else
template< unsigned N > struct AsmNops{
  static void generate() __attribute__((always_inline)){
    __asm__ volatile ("nop.n;");
    AsmNops< N - 1 >::generate();
  }
};
template<> struct AsmNops<0>{ static inline void generate()__attribute__((always_inline)){  __asm__ volatile(";;;"); } };

// loopNezPasses(n): a one-instruction zero-overhead loop, one cycle per
// pass, plus its setup: the movi that loads n (GCC schedules it, but it
// stays in the same straight-line code) and the loopnez itself, 3 cycles in
// all (the loopnez as 2, as tools/core1_cycle_count.py counts it). The
// assembler aligns the loop body itself (it widens or pads the
// instructions before it), so no jump over alignment padding is needed.
// For a runtime count; for a fixed delay use loopNezDelay().
static constexpr int loopNezSetupCycles = 3;
static inline void __attribute__((always_inline)) loopNezPasses(uint32_t passes) {
    __asm__ volatile (
        "loopnez %0, 1f\n\t"
        "nop.n\n\t"
        "1:\n\t"
        : "+r" (passes)
        :
        : "memory"
    );
}

// loopNezDelay(cycles): exactly that many cycles, decided at compile time:
// no instructions for <= 0, that many nop.n up to the loop's setup cost,
// and above it a loopnez of (cycles - setup) passes. A macro because the
// choice needs a constant; cycles must be one.
//
// Even 0 is a scheduling barrier: a volatile asm with no instructions that
// GCC won't move volatile accesses (GPIO reads and writes) across. core 1's
// cycle relies on it: with nothing there, GCC hoisted the address read (P1)
// from cycle 11 to 2, right after the clock poll. The template must not be
// empty: across an empty one GCC still moved it (P1 at 4), but not across
// ";;;" (statement separators, no code; the old AsmNops<0>). Not
// __sync_synchronize(): on Xtensa that is a memw, a real instruction that
// waits for outstanding stores (e.g. the GPIO pin-enable write).
static inline __attribute__((always_inline)) void schedulingBarrier() {
    __asm__ volatile (";;;");
}

template <int Cycles> struct LoopNezDelay {
    static inline __attribute__((always_inline)) void generate() {
        if constexpr (Cycles <= 0) {
            schedulingBarrier();
        } else if constexpr (Cycles <= loopNezSetupCycles) {
            AsmNops<Cycles>::generate();
        } else {
            loopNezPasses(Cycles - loopNezSetupCycles);
        }
    }
};
#define loopNezDelay(cycles) LoopNezDelay<(cycles)>::generate()

// Warning: this is a core0 ifetch hotspot, with a hand-tuned loop delay.
// About ticks * 132 cycles plus the call and loop setup (a runtime count).
static CORE0_HOTSPOTS_ATTR __attribute__((noclone)) void busyWait6502Ticks(uint32_t ticks) {
    static const DRAM_ATTR int ticksPer6502Tick = 132;
    loopNezPasses(ticks * ticksPer6502Tick);
}

static IRAM_ATTR __attribute__((noinline)) void busywait(float sec) {
    uint32_t tsc = XTHAL_GET_CCOUNT();
    static const DRAM_ATTR int cpuFreq = 240 * 1000000;
    while(XTHAL_GET_CCOUNT() - tsc < sec * cpuFreq) {
        AsmNops<30>::generate();
};
}
#endif

#ifndef CSIM_HOOK
#define CSIM_HOOK() while(0) {}
#endif

#define ARRAYSZ(x) (static_cast<int>(sizeof(x)/sizeof(x[0])))
