#include <stdint.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "xtensa/core-macros.h"

#include "mmu.h"
#include "bmon.h"
#include "const.h"
#include "cartridge.h"
#include "extMem.h"
#include "pinDefs.h"

using std::max;
using std::min;

// thoughts for bankL1 changes: 
//   Remove most atariRam[] references, replace with access function that walks the page tables.   Maybe except for 

//DRAM_ATTR uint8_t *pages[nrPages * (1 << PAGESEL_EXTRA_BITS)];
//DRAM_ATTR uint32_t pageEnable[nrPages * (1 << PAGESEL_EXTRA_BITS)];
DRAM_ATTR uint8_t *baseMemPages[nrPages] = {0};

DRAM_ATTR uint8_t atariRam[baseMemSz] = {0x0};
DRAM_ATTR uint8_t dummyRam[pageSize] = {0x0};
DRAM_ATTR uint8_t d000Write[0x800] = {0x0};
DRAM_ATTR uint8_t d000Read[0x800] = {0xff};

DRAM_ATTR uint8_t *screenMem = NULL;
DRAM_ATTR uint8_t pbiROM[0x800] = {
#include "pbirom.h"
};
DRAM_ATTR BankL1Entry banksL1[nrL1Banks * (1 << PAGESEL_EXTRA_BITS)] = {0};
DRAM_ATTR BankL1Entry *banks[nrL1Banks * (1 << PAGESEL_EXTRA_BITS)] = {0};
DRAM_ATTR BankL1Entry dummyBankRd, dummyBankWr, osRomEnabledBank, osRomDisabledBank;

DRAM_ATTR RAM_VOLATILE BankL1Entry *basicEnBankMux[4] = {0};
DRAM_ATTR RAM_VOLATILE BankL1Entry *osEnBankMux[4] = {0};

static const DRAM_ATTR struct {
    uint8_t osEn = 0x1;
    uint8_t basicEn = 0x2;
    uint8_t selfTestEn = 0x80;    
    uint8_t xeBankEn = 0x10;
} portbMask;

IRAM_ATTR void mmuAddBaseRam(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int b = pageNr(start); b <= pageNr(end); b++)  
        baseMemPages[b] = (mem == NULL) ? NULL : mem + ((b - pageNr(start)) * pageSize);
    mmuRemapBaseRam(start, end);

}

IRAM_ATTR uint8_t *mmuAllocAddBaseRam(uint16_t start, uint16_t end) { 
    int pages = pageNr(end) - pageNr(start) + 1;
    uint8_t *mem = (uint8_t *)heap_caps_malloc(pages * pageSize, MALLOC_CAP_INTERNAL);
    assert(mem != NULL);
    bzero(mem, pages * pageSize);
    mmuAddBaseRam(start, end, mem);
    return mem;
}

// Map reads and writes to local SRAM, but allow write accesses to also fall through and write to native RAM 
IRAM_ATTR void mmuMapRangeRW(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            banksL1[page2bank(p + PAGESEL_WR + vid)].pages[p & pageInBankMask] = mem + (p - pageNr(start)) * pageSize;
            banksL1[page2bank(p + PAGESEL_RD + vid)].pages[p & pageInBankMask] = mem + (p - pageNr(start)) * pageSize;
            banksL1[page2bank(p + PAGESEL_WR + vid)].ctrl[p & pageInBankMask] = 0;
            banksL1[page2bank(p + PAGESEL_RD + vid)].ctrl[p & pageInBankMask] = bus.data.mask | bus.extSel.mask;
        }
    }
}

// Map reads and writes to local SRAM, but block write accesses from modifying native RAM 
IRAM_ATTR void mmuMapRangeRWIsolated(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            banksL1[page2bank(p + PAGESEL_WR + vid)].pages[p & pageInBankMask] = mem + (p - pageNr(start)) * pageSize;
            banksL1[page2bank(p + PAGESEL_RD + vid)].pages[p & pageInBankMask] = mem + (p - pageNr(start)) * pageSize;
            banksL1[page2bank(p + PAGESEL_WR + vid)].ctrl[p & pageInBankMask] = bus.extSel.mask;
            banksL1[page2bank(p + PAGESEL_RD + vid)].ctrl[p & pageInBankMask] = bus.data.mask | bus.extSel.mask;
        }
    }
}

// Map reads, block write accesses from modifying native RAM 
IRAM_ATTR void mmuMapRangeRO(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            banksL1[page2bank(p + PAGESEL_WR + vid)].pages[p & pageInBankMask] = &dummyRam[0];
            banksL1[page2bank(p + PAGESEL_RD + vid)].pages[p & pageInBankMask] = mem + (p - pageNr(start)) * pageSize;
            banksL1[page2bank(p + PAGESEL_WR + vid)].ctrl[p & pageInBankMask] = bus.extSel.mask;
            banksL1[page2bank(p + PAGESEL_RD + vid)].ctrl[p & pageInBankMask] = bus.data.mask | bus.extSel.mask;
        }
    }
}

// Unmap range, let accesses fall through to native RAM 
IRAM_ATTR void mmuUnmapRange(uint16_t start, uint16_t end) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) {
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            banksL1[page2bank(p + PAGESEL_WR + vid)].pages[p & pageInBankMask] = &dummyRam[0];
            banksL1[page2bank(p + PAGESEL_RD + vid)].pages[p & pageInBankMask] = &dummyRam[0];
            banksL1[page2bank(p + PAGESEL_WR + vid)].ctrl[p & pageInBankMask] = 0; 
            banksL1[page2bank(p + PAGESEL_RD + vid)].ctrl[p & pageInBankMask] = 0;
        }
    }
}

// Restore any original mapping to local SRAM if it existed.  Allow write accesses to also fall through and write to native RAM 
IRAM_ATTR void mmuRemapBaseRam(uint16_t start, uint16_t end) {
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            //BankL1Entry *b = banksL1[bankToPage(p) + PAGESEL_WR + vid];
            if (baseMemPages[p] != NULL) { 
                banksL1[page2bank(p + PAGESEL_WR + vid)].pages[p & pageInBankMask] = baseMemPages[p];
                banksL1[page2bank(p + PAGESEL_RD + vid)].pages[p & pageInBankMask] = baseMemPages[p];
                banksL1[page2bank(p + PAGESEL_WR + vid)].ctrl[p & pageInBankMask] = 0;
                banksL1[page2bank(p + PAGESEL_RD + vid)].ctrl[p & pageInBankMask] = bus.data.mask | bus.extSel.mask;;
            } else { 
                banksL1[page2bank(p + PAGESEL_WR + vid)].pages[p & pageInBankMask] = &dummyRam[0];
                banksL1[page2bank(p + PAGESEL_RD + vid)].pages[p & pageInBankMask] = &dummyRam[0];
                banksL1[page2bank(p + PAGESEL_WR + vid)].ctrl[p & pageInBankMask] = 0;
                banksL1[page2bank(p + PAGESEL_RD + vid)].ctrl[p & pageInBankMask] = 0;
            }
        }
    }
}

IRAM_ATTR void mmuMapPbiRom(bool pbiEn, bool osEn) {
    if (pbiEn && osEn) {
        mmuMapRangeRWIsolated(_0xd800, _0xdfff, &pbiROM[0]);
        pinReleaseMask &= (~bus.mpd.mask);
        pinDriveMask |= bus.mpd.mask;
    } else if(osEn) {
        mmuUnmapRange(_0xd800, _0xdfff);
        pinReleaseMask |= bus.mpd.mask;
        pinDriveMask &= (~bus.mpd.mask);
    } else {
        mmuRemapBaseRam(_0xd800, _0xdfff);
        pinReleaseMask |= bus.mpd.mask;
        pinDriveMask &= (~bus.mpd.mask);
    }
}

IRAM_ATTR void mmuUnmapBank(uint16_t addr) { 
    for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
        banks[page2bank(pageNr(addr) | vid | PAGESEL_RD)] = &dummyBankRd;
        banks[page2bank(pageNr(addr) | vid | PAGESEL_WR)] = &dummyBankWr;
    }
}
IRAM_ATTR void mmuMapBankRO(uint16_t addr, BankL1Entry *b) { 
    for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
        banks[page2bank(pageNr(addr) | vid | PAGESEL_RD)] = b; 
        banks[page2bank(pageNr(addr) | vid | PAGESEL_WR)] = &dummyBankWr;
    }   
}
IRAM_ATTR void mmuRemapBankBaseRam(uint16_t addr) { 
    for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
        banks[page2bank(pageNr(addr) | vid | PAGESEL_RD)] = &banksL1[page2bank(pageNr(addr) | vid | PAGESEL_RD)]; 
        banks[page2bank(pageNr(addr) | vid | PAGESEL_WR)] = &banksL1[page2bank(pageNr(addr) | vid | PAGESEL_WR)]; 
    }
}

// Called any time values in portb(0xd301) or newport(0xd1ff) change
IRAM_ATTR void mmuOnChange(bool force /*= false*/) {
    uint32_t stsc = XTHAL_GET_CCOUNT();
    mmuChangeBmonMaxStart = max((bmonHead - bmonTail) & bmonArraySzMask, mmuChangeBmonMaxStart); 
    uint8_t newport = d000Write[_0x1ff];
    uint8_t portb = d000Write[_0x301]; 

    static bool lastBasicEn = true;
    static bool lastPbiEn = false;
    static bool lastPostEn = false;
    static bool lastOsEn = true;
    static bool lastXeBankEn = false;
    static int lastXeBankNr = 0;
    static int lastBankA0 = -1, lastBank80 = -1;

    // Figured this out - the native ram under the bank window can't be used usable during
    // bank switching becuase it catches the writes to extended ram and gets corrupted. 
    // Once a sparse base memory map is implemented, we will need to leave this 16K
    // mapped to emulated RAM.  
    bool postEn = (portb & portbMask.selfTestEn) == 0;
    bool xeBankEn = (portb & portbMask.xeBankEn) == 0;
    int xeBankNr = ((portb & 0x60) >> 3) | ((portb & 0x0c) >> 2); 
    if (lastXeBankEn != xeBankEn ||  lastXeBankNr != xeBankNr || force) { 
        uint8_t *mem;
        if (xeBankEn && (mem = extMem.getBank(xeBankNr)) != NULL) { 
            mmuMapRangeRWIsolated(_0x4000, _0x7fff, mem);
        } else { 
            mmuRemapBaseRam(_0x4000, _0x7fff);
        }
        if (postEn) 
            mmuUnmapRange(_0x5000, _0x57ff);
        lastXeBankEn = xeBankEn;
        lastXeBankNr = xeBankNr;
    }

    bool osEn = (portb & portbMask.osEn) != 0;
    bool pbiEn = (newport & pbiDeviceNumMask) != 0;
    if (lastOsEn != osEn || force) { 
        if (osEn) {
            mmuUnmapRange(_0xe000, _0xffff);
            mmuUnmapRange(_0xc000, _0xcfff);
        } else { 
            mmuRemapBaseRam(_0xe000, _0xffff);
            mmuRemapBaseRam(_0xc000, _0xcfff);
        }
        if (0) { 
            // TODO: Why does this hang TurboBasicXL, seems like when its trying to use floating point?
            mmuMapPbiRom(pbiEn, osEn);
            lastPbiEn = pbiEn;
        }
        lastOsEn = osEn;
    }

    if (pbiEn != lastPbiEn || force) {
        mmuMapPbiRom(pbiEn, osEn);
        lastPbiEn = pbiEn;
    }

    if (lastPostEn != postEn || force) { 
        uint8_t *mem;
        if (postEn) {
            mmuUnmapRange(_0x5000, _0x57ff);
        } else if (xeBankEn && (mem = extMem.getBank(xeBankNr)) != NULL) { 
            mmuMapRangeRWIsolated(_0x4000, _0x7fff, mem);
        } else { 
            mmuRemapBaseRam(_0x5000, _0x57ff);
        }
        lastPostEn = postEn;
    }

    bool basicEn = (portb & portbMask.basicEn) == 0;
    if (lastBasicEn != basicEn || lastBankA0 != atariCart.bankA0 || force) { 
        if (basicEn) { 
            mmuUnmapBank(_0xa000);
            //mmuUnmapRange(_0xa000, 0xbfff);
        } else if (atariCart.bankA0 >= 0) {
            mmuMapBankRO(_0xa000, &atariCart.image[atariCart.bankA0].mmuData);
        } else { 
            mmuRemapBankBaseRam(_0xa000);
            //mmuRemapBaseRam(_0xa000, 0xbfff);
        }
        lastBasicEn = basicEn;
        lastBankA0 = atariCart.bankA0;
    }
    if (lastBank80 != atariCart.bank80 || force) { 
        if (atariCart.bank80 >= 0) {
            mmuMapRangeRO(_0x8000, _0x9fff, atariCart.image[atariCart.bank80].mem);
        } else { 
            mmuRemapBaseRam(_0x8000, _0x9fff);
        }
        lastBank80 = atariCart.bank80;
    }
}

// verify the a8 address range is mapped to internal esp32 ram and is continuous 
IRAM_ATTR uint8_t *mmuCheckRangeMapped(uint16_t addr, uint16_t len) { 
    for(int p = pageNr(addr); p <= pageNr(addr + len - 1); p++) { 
        if (banksL1[page2bank(p + PAGESEL_RD + PAGESEL_CPU)].ctrl[p & pageInBankMask] == 0) 
            return NULL;
        if (banksL1[page2bank(p + PAGESEL_WR + PAGESEL_CPU)].pages[p & pageInBankMask] == &dummyRam[0]) 
            return NULL;
        // check mapping is continuous 
        uint8_t *firstPageMem = banksL1[page2bank(pageNr(addr) + PAGESEL_WR + PAGESEL_CPU)].pages[pageNr(addr) & pageInBankMask];
        int offset = (p - pageNr(addr)) * pageSize;
        if (banksL1[page2bank(p + PAGESEL_WR + PAGESEL_CPU)].pages[p & pageInBankMask] != firstPageMem + offset)
            return NULL;
    }
    return banksL1[page2bank(pageNr(addr) + PAGESEL_WR + PAGESEL_CPU)].pages[pageNr(addr) & pageInBankMask] + (addr & pageOffsetMask);
}

IRAM_ATTR void mmuInit() { 
    for(int b = 0; b < nrL1Banks * (1 << PAGESEL_EXTRA_BITS); b++) 
        banks[b] = &banksL1[b];

    basicEnBankMux[0] = &dummyBankWr;
    for(int p = 0; p < pagesPerBank; p++) { // set up dummyBankWd and dummyBankWr bank table entries  
        dummyBankRd.pages[p] = &dummyRam[0];
        dummyBankWr.pages[p] = &dummyRam[0];
        dummyBankWr.ctrl[p] = 0;  
        dummyBankRd.ctrl[p] = 0;
    }
    basicEnBankMux[0] = &dummyBankRd;
    basicEnBankMux[1] = &banksL1[page2bank(pageNr(0xa000) + PAGESEL_RD + PAGESEL_CPU)];

    mmuUnmapRange(0x0000, 0xffff);
    mmuAddBaseRam(0x0000, baseMemSz - 1, atariRam);
    if (baseMemSz < 0x8000) 
        mmuAllocAddBaseRam(0x4000, 0x7fff);
    if (baseMemSz < 0xe000) 
        mmuAllocAddBaseRam(0xd800, 0xdfff);
    mmuUnmapRange(0xd000, 0xd7ff);

    // map register writes for d000-d7ff to shadow write pages
    for(int p = pageNr(0xd000); p <= pageNr(0xd7ff); p++) { 
        banksL1[page2bank(p + PAGESEL_WR + PAGESEL_CPU)].pages[p & pageInBankMask] = &d000Write[0] + (p - pageNr(0xd000)) * pageSize; 
    }
    
#if pageSize <= 0x100    
    // enable reads from 0xd500-0xd5ff for emulating RTC-8 and other cartsel features 
    for(int p = pageNr(0xd500); p <= pageNr(0xd5ff); p++) { 
        banksL1[page2bank(p + PAGESEL_CPU + PAGESEL_RD)].pages[p & pageInBankMask] = &d000Write[(p - pageNr(0xd000)) * pageSize]; 
        banksL1[page2bank(p + PAGESEL_CPU + PAGESEL_RD)].ctrl[p & pageInBankMask] = bus.data.mask | bus.extSel.mask;
    }

    // Map register reads for the page containing 0xd1ff so we can handle reads to newport/0xd1ff 
    // implementing PBI interrupt scheme 
    banksL1[page2bank(pageNr(0xd1ff) | PAGESEL_CPU | PAGESEL_RD)].pages[pageNr(0xd1ff) & pageInBankMask] = &d000Read[(pageNr(0xd1ff) - pageNr(0xd000)) * pageSize]; 
    banksL1[page2bank(pageNr(0xd1ff) | PAGESEL_CPU | PAGESEL_RD)].ctrl[pageNr(0xd1ff) & pageInBankMask] = bus.data.mask | bus.extSel.mask;
    
    // technically should support cartctl reads also
    // pageEnable[pageNr(0xd500) | PAGESEL_CPU | PAGESEL_RD ] |= pins.halt.mask;
#endif

    // enable the halt(ready) line in response to writes to 0xd301, 0xd1ff or 0xd500
    banksL1[page2bank(pageNr(0xd1ff) | PAGESEL_CPU | PAGESEL_WR)].ctrl[pageNr(0xd1ff) & pageInBankMask] |= bus.halt_.mask;
    banksL1[page2bank(pageNr(0xd301) | PAGESEL_CPU | PAGESEL_WR)].ctrl[pageNr(0xd301) & pageInBankMask] |= bus.halt_.mask;
    //banksL1[page2bank(pageNr(0xd500) | PAGESEL_CPU | PAGESEL_WR)].ctrl[pageNr(0xd500) & pageInBankMask] |= bus.halt_.mask;

    // Intialize register shadow write memory to the default hardware reset values
    d000Write[0x301] = 0xff;
    d000Write[0x1ff] = 0x00;
    d000Read[0x1ff] = 0x00;
    mmuOnChange(/*force =*/true);

    for(int p = 0; p < ARRAYSZ(cartBanks); p++) 
        cartBanks[p] = &banksL1[page2bank(pageNr(0xa000) | PAGESEL_CPU | PAGESEL_RD)];
    for(int b = 0; b < atariCart.bankCount; b++) 
        cartBanks[b] = &atariCart.image[b].mmuData;

    mmuRemapBaseRam(_0xe000, _0xffff);
    mmuRemapBaseRam(_0xc000, _0xcfff);
    osRomDisabledBank = banksL1[page2bank(pageNr(0xc000) + PAGESEL_RD + PAGESEL_CPU)];
    osEnBankMux[0] = &osRomDisabledBank;

    mmuUnmapRange(_0xe000, _0xffff);
    mmuUnmapRange(_0xc000, _0xcfff);
    osRomEnabledBank = banksL1[page2bank(pageNr(0xc000) + PAGESEL_RD + PAGESEL_CPU)];
    osEnBankMux[1] = &osRomEnabledBank;
}
