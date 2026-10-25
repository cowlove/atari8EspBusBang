#pragma once 
#include <inttypes.h>
#include "esp_attr.h"
#include "xtensa/core-macros.h"
#include "core0_hotspots.h"
#pragma GCC diagnostic ignored "-Wunused-function"

static IRAM_ATTR void screenMemToAscii(char *buf, int buflen, char c) { 
    bool inv = false;
    if (c & 0x80) {
        c -= 0x80;
        inv = true;
    };
    if (c < 64) c += 32;
    else if (c < 96) c -= 64;
    if (inv) 
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

static inline void loopNezDelay(uint32_t iterations) { (void)iterations; }
static CORE0_HOTSPOTS_ATTR void busyWait6502Ticks(uint32_t ticks) { (void)ticks; }
static inline void busywait(float sec) { (void)sec; }
#else
template< unsigned N > struct AsmNops{
  static void generate() __attribute__((always_inline)){
    __asm__ volatile ("nop;");
    AsmNops< N - 1 >::generate();
  }
};
template<> struct AsmNops<0>{ static inline void generate()__attribute__((always_inline)){  __asm__ volatile(";;;"); } };

static inline void __attribute__((always_inline)) loopNezDelay(uint32_t iterations) {
    __asm__ volatile (
        "j 2f\n\t"
        ".align 4\n\t"
        "2:\n\t"
        "loopnez %0, 1f\n\t"
        "nop.n\n\t"
        "nop.n\n\t"
        "1:\n\t"
        : "+r" (iterations)
        :
        : "memory"
    );
}

// Warning: this is a core0 ifetch hotspot, with a hand-tuned loop delay.
static CORE0_HOTSPOTS_ATTR __attribute__((noclone)) void busyWait6502Ticks(uint32_t ticks) {
    static const DRAM_ATTR int ticksPer6502Tick = 132;
    loopNezDelay(ticks * (ticksPer6502Tick / 2));
}

static IRAM_ATTR __attribute__((noinline)) void busywait(float sec) {
    uint32_t tsc = XTHAL_GET_CCOUNT();
    static const DRAM_ATTR int cpuFreq = 240 * 1000000;
    while(XTHAL_GET_CCOUNT() - tsc < sec * cpuFreq) {
        AsmNops<30>::generate();
};
}
#endif

#define ARRAYSZ(x) (sizeof(x)/sizeof(x[0]))
