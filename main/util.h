#pragma once 
#include <inttypes.h>
#include "esp_attr.h"
#include "xtensa/core-macros.h"

static inline void screenMemToAscii(char *buf, int buflen, char c) { 
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

static inline void delayTicks(int ticks) { 
    uint32_t startTsc = XTHAL_GET_CCOUNT();
    while(XTHAL_GET_CCOUNT() - startTsc < ticks) {}
}


static inline void busyWaitTicks(uint32_t ticks) { 
    uint32_t tsc = XTHAL_GET_CCOUNT();
    while(XTHAL_GET_CCOUNT() - tsc < ticks) {};
}
static inline void busyWait6502Ticks(uint32_t ticks) { 
    uint32_t tsc = XTHAL_GET_CCOUNT();
    static const DRAM_ATTR int ticksPer6502Tick = 132;
    while(XTHAL_GET_CCOUNT() - tsc < ticks * ticksPer6502Tick) {};
}
static inline void busywait(float sec) {
    uint32_t tsc = XTHAL_GET_CCOUNT();
    static const DRAM_ATTR int cpuFreq = 240 * 1000000;
    while(XTHAL_GET_CCOUNT() - tsc < sec * cpuFreq) {};
}
