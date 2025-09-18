#include <cinttypes>
#define DRAM_ATTR 
#define IRAM_ATTR
#define FLASH_ATTR
#define DRAM_STR(x) x
#define XTHAL_GET_CCOUNT() 0

uint8_t atariRam[64 * 1024] = {0};