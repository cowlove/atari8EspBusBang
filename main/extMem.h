#pragma once
//#pragma GCC optimize("O1")

#include <inttypes.h>
#include <new>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "bmon.h"
#include "util.h"
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

// AUTO's bank layout (docs/memory-autodetect.md), a pure function so it can
// be tested on its own. premap[] is indexed by PORTB bits 2-6: bits 0-1 =
// PORTB 2-3 (bank within a set of 4), bit 2 = PORTB 4 (0: CPU sees the
// bank), bits 3-4 = PORTB 5-6 ("g", which set position). A set is 4 banks.
// Native sets (nativeBanks / 4, from the memory probe) take positions as
// the hardware does: a 130XE answers its 4 banks at every g, and the board
// claims positions over it with extSel_.
//  - Halt line and PSRAM: fill out to RAMBO256. g = 0 is main memory, or
//    the first native set; the board claims g = 1..3 (after any further
//    native sets), PSRAM-backed, fronted by min(sram budget, banks) SRAM
//    banks; none at all without SRAM for one.
//  - Otherwise SRAM only, whole sets: the largest of stock XE (1 position,
//    mirrored on all g), Compy 192 (2: bit 6 = 0 / 1) and 4 positions (one
//    per g) whose board sets fit the budget. Native sets take the last
//    positions, so a 130XE with 4 SRAM banks gets NATIVE_XE_COMPY192's map.
struct ExtMemAutoPlan {
    int banks = 0, sramBanks = 0;
    int premap[32];
};
static inline ExtMemAutoPlan extMemPlanAuto(int sramBudget, bool halt, bool psram, int nativeBanks) {
    ExtMemAutoPlan p;
    for (int &m : p.premap) m = -1;
    const int nativeSets = nativeBanks / 4;
    int sets = 0;
    auto claim = [&](unsigned gMask) {   // bit g: PORTB bits 5-6 == g
        for (int g = 0; g < 4; g++)
            if (gMask & (1u << g))
                for (int i = 0; i < 4; i++) p.premap[i | (g << 3)] = sets * 4 + i;
        sets++;
    };
    if (halt && psram) {
        if (sramBudget < 1) return p;
        for (int g = nativeSets > 1 ? nativeSets : 1; g < 4; g++) claim(1u << g);
        p.banks = sets * 4;
        p.sramBanks = sramBudget < p.banks ? sramBudget : p.banks;
        return p;
    }
    static const unsigned positions[3][4] = {{0xf}, {0x3, 0xc}, {0x1, 0x2, 0x4, 0x8}};
    static const int nPositions[3] = {1, 2, 4};
    for (int level = 2; level >= 0; level--) {
        const int boardSets = nPositions[level] - nativeSets;
        if (boardSets <= 0 || boardSets * 4 > sramBudget) continue;
        for (int s = 0; s < boardSets; s++) claim(positions[level][s]);
        break;
    }
    p.banks = p.sramBanks = sets * 4;
    return p;
}

class ExtBankPool {
    int totalBanks = 0, sramBanks = 0;
    int *recency = nullptr;
    DRAM_ATTR static const int bankSz = 0x4000;
public: 
    int premap[32] = {-1};
    uint8_t **banks = nullptr;
    uint8_t *spare = NULL;
    enum ExtMemConfig { 
        NONE = 0,
        RAMBO256,
        NATIVE_XE_COMPY192,
        XE,
        AUTO   // init()'s haltAvailable and nativeBanks pick the layout (extMemPlanAuto())
    };
    int evictCount = 0, swapCount = 0;
    void release() {
        if (banks) {
            for (int i = 0; i < totalBanks; i++)
                if (banks[i]) heap_caps_free(banks[i]);
            delete[] banks;
        }
        if (recency) delete[] recency;
        if (spare) heap_caps_free(spare);
        banks = nullptr;
        recency = nullptr;
        spare = nullptr;
        totalBanks = 0;
        sramBanks = 0;
        evictCount = 0;
        swapCount = 0;
        mapNone();
    }

    // n banks, the first sram in SRAM, the rest in PSRAM, laid out by config.
    // AUTO: sram is the SRAM budget in banks; the plan sets n and the SRAM
    // count from haltAvailable, the free PSRAM and nativeBanks (the probe's
    // count of distinct native banks).
    // TODO: the explicit layouts allocate all n (16) banks and a spare, used
    // or not; allocate only what the layout maps.
    bool init(int n, int sram, ExtMemConfig config, bool haltAvailable = false, int nativeBanks = 0) {
        release();
        ExtMemAutoPlan plan;
        if (config == AUTO) {
            const bool psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= size_t(13 * bankSz);
            plan = extMemPlanAuto(sram, haltAvailable, psram, nativeBanks);
            printf("ExtBankPool AUTO: halt %d, PSRAM %d, %d native banks, SRAM budget %d: "
                   "%d banks, %d in SRAM\n", haltAvailable, psram, nativeBanks, sram,
                   plan.banks, plan.sramBanks);
            if (plan.banks == 0) return true;   // native only (premap all -1)
            n = plan.banks;
            sram = plan.sramBanks;
        }
        if (n <= 0 || sram < 0 || sram > n) {
            printf("ExtBankPool::init: invalid bank counts (%d total, %d SRAM)\n", n, sram);
            return false;
        }
        totalBanks = n;
        sramBanks = sram;
        // TODO: allocate only the banks required by the requested expansion scheme.
        banks = new (std::nothrow) uint8_t *[n]();
        recency = new (std::nothrow) int[n]();
        if (!banks || !recency) {
            printf("ExtBankPool::init: bank metadata allocation failed\n");
            release();
            return false;
        }
        // allocate the first banks in DRAM 
        for(int n = 0; n < sramBanks; n++) { 
            banks[n] = (uint8_t *)heap_caps_malloc(bankSz, MALLOC_CAP_INTERNAL);
            if (!banks[n]) {
                printf("ExtBankPool::init: SRAM bank %d allocation failed\n", n);
                release();
                return false;
            }
            bzero(banks[n], bankSz);
            recency[n] = n;
        }
        // allocate the rest in PSRAM
        for(int n = sramBanks; n < totalBanks; n++) { 
            banks[n] = (uint8_t *)heap_caps_malloc(bankSz, MALLOC_CAP_SPIRAM);
            if (!banks[n]) {
                printf("ExtBankPool::init: PSRAM bank %d allocation failed\n", n);
                release();
                return false;
            }
            bzero(banks[n], bankSz);
        }
        if (totalBanks > sramBanks) {
            spare = (uint8_t *)heap_caps_malloc(bankSz, MALLOC_CAP_SPIRAM);
            if (!spare) {
                printf("ExtBankPool::init: PSRAM spare bank allocation failed\n");
                release();
                return false;
            }
            bzero(spare, bankSz);
        }
        switch (config) {
            case RAMBO256:
                mapRambo256();
                break;
            case NATIVE_XE_COMPY192:
                mapNativeXe192();
                break;
            case XE:
                mapStockXE();
                break;
            case AUTO:
                for (int i = 0; i < ARRAYSZ(premap); i++) premap[i] = plan.premap[i];
                break;
            case NONE:
            default:
                mapNone();
                break;
        }
        for(int i = 0; i < ARRAYSZ(premap); i++) // exercise all banks to pre-load mapped ones into SRAM
            getBank(i);
        evictCount = 0;
        return true;
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
        for(int i = 0; i < 4; i++) {
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
        for(int i = 0; i < ARRAYSZ(premap); i++) premap[i] = -1;
    }

    IRAM_ATTR inline void memcpy(uint8_t *dst, uint8_t *src, int len) { 
        for(int n = 0; n < len; n++) dst[n] = src[n];
    }
    IRAM_ATTR inline bool isSRAM(int b) {
        // csim: host heap pointers all pass, so every bank is "SRAM" and
        // nothing is evicted (csim can't halt the 6502 for a swap anyway).
        static const DRAM_ATTR uint8_t *cutoff = (uint8_t *)0x3f000000;
        return banks[b] >= cutoff;
    }
    IRAM_ATTR uint8_t *getBank(int b);
};

extern DRAM_ATTR ExtBankPool extMem; 
