#pragma once
//#pragma GCC optimize("O1")

#include <inttypes.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "bmon.h"
#include "xtensa/core-macros.h"

using std::min;
using std::max;

// ExtBankPool manages 16K banks of memory to implement bank switching.  
// banks[] keeps an array of pointers to 16K memory blocks.  These banks are used to populate 
// the mmuConfig.extMemBanks[] array.

// If these banks are all in SRAM, that works fine, and the core1 loop can handle bank swapping
// without any help. 

// However, if PSRAM banks are used to extend the amount of extended ram, PSRAM is too slow for
// core1 to use.  In thise case, writes to 0xd301 are trapped by setting the halt_ bit on the 0xd0
// page, and the core0 loop calls getBank(), which checks to see if the selected bank is PSRAM. 
// If it is, it copy/swaps it with the least recently used SRAM bank, updates mmuConfig.extMemBanks[] entry
// to point to the new SRAM, and only then lets the 6502 continue.  

class ExtBankPool {
    int totalBanks, sramBanks;
    int *recency;
    uint8_t *spare = NULL;
    DRAM_ATTR static const int bankSz = 0x4000;
public: 
    int premap[32] = {-1};
    uint8_t **banks;

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
        mapNone();
    }

    void mapStockXL() {  
        mapNone();
    }
     
    void mapStockXE() {
        // E banking   
        mapNone();
        for(int i = 0; i < 4; i++) { 
            premap[i + 0b00000] = i;
            premap[i + 0b01000] = i;
            premap[i + 0b10000] = i;
            premap[i + 0b11000] = i;
        }
    }

    void mapRambo256() {
        mapNone();
        //8ACE banking with 8 block aliasing base memory
        assert(false); // TODO fix this to use 5-bit bank num 
        for(int i = 1; i < 4; i++) {
            premap[i + 0b01000] = i + 0;
            premap[i + 0b10000] = i + 4;
            premap[i + 0b11000] = i + 8;
        }
    }
    
    void mapCompy192() {
        mapNone();
        for(int i = 0; i < 4; i++) { 
            premap[i + 0b00000] = i;
            premap[i + 0b01000] = i;
            premap[i + 0b10000] = i + 4;
            premap[i + 0b11000] = i + 4;
        }
    }

    void mapNativeXe192() {  
        mapNone();
        for(int i = 0; i < 4; i++) { 
            premap[i + 0b00000] = i;
            premap[i + 0b01000] = i;
        }
    }

    void mapNone() { 
        for(int i = 0; i < 32; i++) premap[i] = -1;
    }

    IRAM_ATTR inline void memcpy(uint8_t *dst, uint8_t *src, int len) { 
        for(int n = 0; n < len; n++) dst[n] = src[n];
    }
    IRAM_ATTR inline bool isSRAM(int b) {
        static const DRAM_ATTR uint8_t *cutoff = (uint8_t *)0x3f000000;
        return banks[b] > cutoff;
    }
    IRAM_ATTR uint8_t *getBank(int b);
};

extern DRAM_ATTR ExtBankPool extMem; 
