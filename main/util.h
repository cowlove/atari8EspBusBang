#pragma once 
#include <inttypes.h>
#include "esp_attr.h"
#include "xtensa/core-macros.h"
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
template< unsigned N > struct AsmNops{
  static void generate() __attribute__((always_inline)){
    __asm__ volatile ("nop;");
    AsmNops< N - 1 >::generate();
  }
};
template<> struct AsmNops<0>{ static inline void generate()__attribute__((always_inline)){  __asm__ volatile(";;;"); } };

static inline IRAM_ATTR void delayTicks(int ticks) { 
    uint32_t startTsc = XTHAL_GET_CCOUNT();
    while(XTHAL_GET_CCOUNT() - startTsc < ticks) {}
}

static inline  IRAM_ATTR void busyWaitTicks(uint32_t ticks) { 
    uint32_t tsc = XTHAL_GET_CCOUNT();
    while(XTHAL_GET_CCOUNT() - tsc < ticks) {};
}
#define NOP_10()   asm volatile("memw; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;")
#define NOP_100()  NOP_10() NOP_10() NOP_10() NOP_10() NOP_10() NOP_10() NOP_10() NOP_10() NOP_10() NOP_10()
static IRAM_ATTR inline void busyWait6502Ticks(uint32_t ticks) { 
    uint32_t tsc = XTHAL_GET_CCOUNT();
    static const DRAM_ATTR int ticksPer6502Tick = 132;
    while(XTHAL_GET_CCOUNT() - tsc < ticks * ticksPer6502Tick) { 
        { AsmNops<100>::generate(); }
    };
}
static inline IRAM_ATTR void busywait(float sec) {
    uint32_t tsc = XTHAL_GET_CCOUNT();
    static const DRAM_ATTR int cpuFreq = 240 * 1000000;
    while(XTHAL_GET_CCOUNT() - tsc < sec * cpuFreq) {};
}

#define ARRAYSZ(x) (sizeof(x)/sizeof(x[0]))
