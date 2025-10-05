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

DRAM_ATTR uint8_t *pages[nrPages * (1 << PAGESEL_EXTRA_BITS)];
DRAM_ATTR uint32_t pageEnable[nrPages * (1 << PAGESEL_EXTRA_BITS)];
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

static const DRAM_ATTR struct {
    uint8_t osEn = 0x1;
    uint8_t basicEn = 0x2;
    uint8_t selfTestEn = 0x80;    
    uint8_t xeBankEn = 0x10;
} portbMask;

IRAM_ATTR void mmuAddBaseRam(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int b = pageNr(start); b <= pageNr(end); b++)  
        baseMemPages[b] = (mem == NULL) ? NULL : mem + ((b - pageNr(start)) * pageSize);
}

IRAM_ATTR uint8_t *mmuAllocAddBaseRam(uint16_t start, uint16_t end) { 
    int pages = pageNr(end) - pageNr(start) + 1;
    uint8_t *mem = (uint8_t *)heap_caps_malloc(pages * pageSize, MALLOC_CAP_INTERNAL);
    assert(mem != NULL);
    bzero(mem, pages * pageSize);
    mmuAddBaseRam(start, end, mem);
    return mem;
}

IRAM_ATTR void mmuMapRangeRW(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : {PAGESEL_CPU, PAGESEL_VID}) {  
            pages[p + PAGESEL_WR + vid] = mem + (p - pageNr(start)) * pageSize;
            pages[p + PAGESEL_RD + vid] = mem + (p - pageNr(start)) * pageSize;
            pageEnable[p + vid + PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
            pageEnable[p + vid + PAGESEL_WR] = 0; // no bus.extSel.mask, let writes go through to native mem 
        }
    }
}

IRAM_ATTR void mmuMapRangeRWIsolated(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : {PAGESEL_CPU, PAGESEL_VID}) {  
            pages[p + PAGESEL_WR + vid] = mem + (p - pageNr(start)) * pageSize;
            pages[p + PAGESEL_RD + vid] = mem + (p - pageNr(start)) * pageSize;
            pageEnable[p + vid + PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
            pageEnable[p + vid + PAGESEL_WR] = bus.extSel.mask;
        }
    }
}

IRAM_ATTR void mmuMapRangeRO(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : {PAGESEL_CPU, PAGESEL_VID}) {  
            pages[p + PAGESEL_WR + vid] = &dummyRam[0];
            pages[p + PAGESEL_RD + vid] = mem + (p - pageNr(start)) * pageSize;
            pageEnable[p + vid + PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
            pageEnable[p + vid + PAGESEL_WR] = bus.extSel.mask;
        }
    }
}

IRAM_ATTR void mmuUnmapRange(uint16_t start, uint16_t end) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) {
        for(int vid : {PAGESEL_CPU, PAGESEL_VID}) {  
            pages[p + PAGESEL_WR + vid] = &dummyRam[0];
            pages[p + PAGESEL_RD + vid] = &dummyRam[0];
            pageEnable[p + PAGESEL_RD + vid] = 0;
            pageEnable[p + PAGESEL_WR + vid] = 0;
        }
    }
}

IRAM_ATTR void mmuRemapBaseRam(uint16_t start, uint16_t end) {
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : {PAGESEL_CPU, PAGESEL_VID}) {  
            //BankL1Entry *b = banksL1[bankToPage(p) + PAGESEL_WR + vid];
            if (baseMemPages[p] != NULL) { 
                pages[p + PAGESEL_WR + vid] = baseMemPages[p];
                pages[p + PAGESEL_RD + vid] = baseMemPages[p];
                pageEnable[p + vid + PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
                pageEnable[p + vid + PAGESEL_WR] = 0; // no bus.extSel.mask, let writes go through to native mem
            } else { 
                pages[p + PAGESEL_WR + vid] = &dummyRam[0];
                pages[p + PAGESEL_RD + vid] = &dummyRam[0];
                pageEnable[p + vid + PAGESEL_RD] = 0;
                pageEnable[p + vid + PAGESEL_WR] = 0;
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
            mmuUnmapRange(_0xa000, _0xbfff);
        } else if (atariCart.bankA0 >= 0) {
            mmuMapRangeRO(_0xa000, _0xbfff, atariCart.image[atariCart.bankA0]);
        } else { 
            mmuRemapBaseRam(_0xa000, _0xbfff);
        }
        lastBasicEn = basicEn;
        lastBankA0 = atariCart.bankA0;
    }
    if (lastBank80 != atariCart.bank80 || force) { 
        if (atariCart.bank80 >= 0) {
            mmuMapRangeRO(_0x8000, _0x9fff, atariCart.image[atariCart.bank80]);
        } else { 
            mmuRemapBaseRam(_0x8000, _0x9fff);
        }
        lastBank80 = atariCart.bank80;
    }
}

// verify the a8 address range is mapped to internal esp32 ram and is continuous 
IRAM_ATTR uint8_t *mmuCheckRangeMapped(uint16_t addr, uint16_t len) { 
    for(int b = pageNr(addr); b <= pageNr(addr + len - 1); b++) { 
        if (pageEnable[PAGESEL_CPU + PAGESEL_RD + b] == 0) 
            return NULL;
        if (pages[PAGESEL_CPU + PAGESEL_WR + b] == &dummyRam[0]) 
            return NULL;
        // check mapping is continuous 
        uint8_t *firstPageMem = pages[PAGESEL_CPU + PAGESEL_WR + pageNr(addr)];
        int offset = (b - pageNr(addr)) * pageSize;
        if (pages[PAGESEL_CPU + PAGESEL_WR + b] != firstPageMem + offset)
            return NULL;
    }
    return pages[pageNr(addr) + PAGESEL_CPU + PAGESEL_RD] + (addr & pageOffsetMask);
}

IRAM_ATTR void mmuInit() { 
    bzero(pageEnable, sizeof(pageEnable));
    mmuUnmapRange(0x0000, 0xffff);

    mmuAddBaseRam(0x0000, baseMemSz - 1, atariRam);
    mmuRemapBaseRam(0x0000, baseMemSz - 1);
    if (baseMemSz < 0x8000) {
        mmuAllocAddBaseRam(0x4000, 0x7fff);
        mmuRemapBaseRam(0x4000, 0x7fff);
    }
    if (baseMemSz < 0xe000) {
        mmuAllocAddBaseRam(0xd800, 0xdfff);
        mmuRemapBaseRam(0xd800, 0xdfff);
    }
    mmuUnmapRange(0xd000, 0xd7ff);

    // map register writes for d000-d7ff to shadow write pages
    for(int b = pageNr(0xd000); b <= pageNr(0xd7ff); b++) { 
        pages[b | PAGESEL_CPU | PAGESEL_WR ] = &d000Write[0] + (b - pageNr(0xd000)) * pageSize; 
    }
    
#if pageSize <= 0x100    
    // enable reads from 0xd500-0xd5ff for emulating RTC-8 and other cartsel features 
    for(int b = pageNr(0xd500); b <= pageNr(0xd5ff); b++) { 
        pages[b | PAGESEL_CPU | PAGESEL_RD ] = &d000Read[0] + (b - pageNr(0xd000)) * pageSize; 
        pageEnable[b | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
    }

    // Map register reads for the page containing 0xd1ff so we can handle reads to newport/0xd1ff 
    // implementing PBI interrupt scheme 
    pages[pageNr(0xd1ff) | PAGESEL_CPU | PAGESEL_RD ] = &d000Read[(pageNr(0xd1ff) - pageNr(0xd000)) * pageSize]; 
    pageEnable[pageNr(0xd1ff) | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
    
    // technically should support cartctl reads also
    // pageEnable[pageNr(0xd500) | PAGESEL_CPU | PAGESEL_RD ] |= pins.halt.mask;
#endif

    // enable the halt(ready) line in response to writes to 0xd301, 0xd1ff or 0xd500
    pageEnable[pageNr(0xd1ff) | PAGESEL_CPU | PAGESEL_WR ] |= bus.halt_.mask;
    pageEnable[pageNr(0xd301) | PAGESEL_CPU | PAGESEL_WR ] |= bus.halt_.mask;
    pageEnable[pageNr(0xd500) | PAGESEL_CPU | PAGESEL_WR ] |= bus.halt_.mask;

    // Intialize register shadow write memory to the default hardware reset values
    d000Write[0x301] = 0xff;
    d000Write[0x1ff] = 0x00;
    d000Read[0x1ff] = 0x00;

    mmuOnChange(/*force =*/true);

    // TMP: sketch in just enough of the bankL1 page tables to let the core1 profiling loop run 
    for(int b = 0; b < nrL1Banks * (1 << PAGESEL_EXTRA_BITS); b++) { 
        //banksL1[b] = &pages[pageNr(b * bankL1Size) * (1 << PAGESEL_EXTRA_BITS)];
        //banksL1Enable[b] = &pageEnable[pageNr(b * bankL1Size) * (1 << PAGESEL_EXTRA_BITS)];
        for(int p = 0; p < pagesPerBank; p++) { 
            banksL1[b].pages[p] = pages[0];
            banksL1[b].ctrl[p] = pageEnable[0];
        }
    }

}
