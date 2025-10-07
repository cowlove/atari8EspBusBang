#include "extMem.h"


IRAM_ATTR uint8_t *ExtBankPool::getBank(int b) {
    b = premap[b]; 
    if (b < 0) return NULL;
    if (!isSRAM(b)) {
        int evict = recency[sramBanks - 1];
        uint8_t *newspare = banks[b];
        this->memcpy(spare, banks[evict], bankSz);
        this->memcpy(banks[evict], banks[b], bankSz);
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
    return banks[b];
}


ExtBankPool extMem; 