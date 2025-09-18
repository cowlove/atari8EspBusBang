#include <cinttypes>
#define DRAM_ATTR 
#define IRAM_ATTR
#define FLASH_ATTR
#define DRAM_STR(x) x
uint32_t XTHAL_GET_CCOUNT();

uint8_t atariRam[64 * 1024] = {0};
uint8_t pbiROM[2 * 1024] = {0};