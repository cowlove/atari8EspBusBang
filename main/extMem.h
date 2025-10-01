#include <inttypes.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"

class ExtBankPool {
    int totalBanks, sramBanks;
    uint8_t **banks;
    int *recency;
    uint8_t *spare = NULL;
    DRAM_ATTR static const int bankSz = 0x4000;
    int premap[16] = {};
public: 
    int evictCount = 0, swapCount = 0;
    void init(int n, int sram) {
        totalBanks = n;
        sramBanks = sram;
        banks = new uint8_t *[n];
        recency = new int[n];
        // allocate the first banks in DRAM 
        for(int n = 0; n < sramBanks; n++) { 
            banks[n] = (uint8_t *)heap_caps_malloc(bankSz, MALLOC_CAP_INTERNAL);
            bzero(banks[n], bankSz);
            recency[n] = n;
        }
        // allocate the rest in PSRAM
        for(int n = sramBanks; n < totalBanks; n++) { 
            banks[n] = (uint8_t *)heap_caps_malloc(bankSz, MALLOC_CAP_SPIRAM);
            bzero(banks[n], bankSz);
        }
        if (totalBanks > sramBanks) {
            spare = (uint8_t *)heap_caps_malloc(bankSz, MALLOC_CAP_SPIRAM);
            bzero(spare, bankSz);
        }
        mapStockXE();
    }

    void mapStockXL() {  
        for(int i = 0; i < 16; i++) 
            premap[i] = -1;
    }
     
    void mapStockXE() {  
        for(int i = 0; i < 4; i++) { 
            premap[i + 0b0000] = i;
            premap[i + 0b0100] = i;
            premap[i + 0b1000] = i;
            premap[i + 0b1100] = i;
        }
    }

    void mapRambo256() {
        for(int i = 0; i < 4; i++) premap[i] = -1;
        for(int i = 4; i < 16; i++) premap[i] = i - 4;
    }
    void mapCompy192() {
        for(int i = 0; i < 4; i++) { 
            premap[i + 0b0000] = i;
            premap[i + 0b0100] = i;
            premap[i + 0b1000] = i + 4;
            premap[i + 0b1100] = i + 4;
        }
    }
    
    IRAM_ATTR inline void memcpy(uint8_t *dst, uint8_t *src, int len) { 
        for(int n = 0; n < len; n++) dst[n] = src[n];
    }
    IRAM_ATTR inline bool isSRAM(int b) {
        static const DRAM_ATTR uint8_t *cutoff = (uint8_t *)0x3f000000;
        return banks[b] > cutoff;
    }
    uint8_t *getBank(int b);
};

extern DRAM_ATTR ExtBankPool extMem; 
