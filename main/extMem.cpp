#ifdef CSIM
#include "csim.h"
#endif

#include "extMem.h"
#include "hotCacheTrap.h"


/*IRAM_ATTR*/ const ExtBank *ExtBankPool::getBank(int b) {
    b = premap[b]; 
    if (b < 0) return NULL;
    if (!isSRAM(b)) {
        // The SRAM bank least recently used goes out to the PSRAM spare, b
        // comes into its SRAM, and the three swap owners: chunk by chunk,
        // the same layout everywhere.
        int evict = recency[sramBanks - 1];
        const ExtBank newspare = banks[b];
        // The one PSRAM access hot core 0 makes by design (the 6502 halted):
        // HOT_CACHE_TRAP builds let it through, the caches resumed for the
        // copies only (hotCacheTrap.h; declared cold calls to the checker).
        // getBank() runs cold too (autoMemConfig()): suspend again only if
        // the caches were suspended (hot).
        const bool trapped = hotCacheResume();
        for (int c = 0; c < ExtBank::chunks; c++) {
            this->memcpy(spare.chunk[c], banks[evict].chunk[c], ExtBank::chunkSz);
            this->memcpy(banks[evict].chunk[c], banks[b].chunk[c], ExtBank::chunkSz);
        }
        if (trapped) hotCacheSuspend();
        banks[b] = banks[evict];
        banks[evict] = spare;
        spare = newspare; 
        // evict was at the bottom of the list, move whole list down and insert b at top 
        for(int i = sramBanks - 1; i > 0; i--) {  
            recency[i] = recency[i - 1];
        }
        recency[0] = b;
        evictCount++;
    } else {                 
        // find b and insert it at the top of the recency list
        for(int i = 0; i < sramBanks; i++) { 
            if (recency[i] == b) {
                for(int j = i - 1; j >= 0; j--) {
                    recency[j + 1] = recency[j];
                }
                break;
            }
        }
        recency[0] = b;
    }
    swapCount++;
    return &banks[b];
}


ExtBankPool extMem; 
