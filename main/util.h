#pragma once 
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