#ifdef CSIM
#include "csim.h"
#endif

#include <stdint.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "xtensa/core-macros.h"

#include "mmu.h"
#include "sfmt.h"
#include "sysConfig.h"
#include "bmon.h"
#include "const.h"
#include "cartridge.h"
#include "extMem.h"
#include "pinDefs.h"

using std::max;
using std::min;

// Thoughts for L1 block changes:
//   Remove most atariRam[] references, replace with access function that walks the page tables.   Maybe except for 

DRAM_ATTR uint8_t *baseMemPages[nrPages] = {0};
DRAM_ATTR uint8_t *atariRam = NULL;
DRAM_ATTR uint8_t dummyRam[pageSize] = {0x0};
DRAM_ATTR uint8_t d000Write[0x800] = {0x0};
DRAM_ATTR uint8_t d000Read[0x800] = {0xff};

DRAM_ATTR uint8_t *screenMem = NULL;
extern "C" DRAM_ATTR uint8_t pbiROM[0x800] = {
#include "pbirom.h"
};
// L1 blocks are 1.5K each; four entries use about 6K.
DRAM_ATTR L1Block l1Blocks[nrL1Blocks] = {0}; // 6K
DRAM_ATTR L1Block basicEnabledL1Block, basicDisabledL1Block, osRomEnabledL1Block, osRomEnabledPbiL1Block, osRomDisabledL1Block, dummyL1BlockWithHalt, dummyL1Block; // 9K
//DRAM_ATTR L1Block extMemBanks[32]; // 48K

//static constexpr int x = sizeof(l1Blocks);
DRAM_ATTR RAM_VOLATILE MmuState mmuState;
DRAM_ATTR RAM_VOLATILE MmuState mmuStateSaved;
DRAM_ATTR RAM_VOLATILE MmuState mmuStateDisabled;
DRAM_ATTR int baseMemSz = 2 * 1024;

DRAM_ATTR BUSCTL_VOLATILE uint32_t pinReleaseMask = bus.irq_.mask | bus.data.mask | bus.extSel.mask | bus.mpd.mask | bus.halt_.mask;

//DRAM_ATTR RAM_VOLATILE L1Block *basicEnL1BlockMux[2] = {0};
//DRAM_ATTR RAM_VOLATILE L1Block *osEnL1BlockMux[4] = {0};

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
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) + PAGESEL_WR + vid] = mem + (p - pageNr(start)) * pageSize;
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) + PAGESEL_RD + vid] = mem + (p - pageNr(start)) * pageSize;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) + PAGESEL_WR + vid] = 0;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) + PAGESEL_RD + vid] = bus.data.mask | bus.extSel.mask;
        }
    }
}

// Map reads and writes to local SRAM, but block write accesses from modifying native RAM 
IRAM_ATTR void mmuMapRangeRWIsolated(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) + PAGESEL_WR + vid] = mem + (p - pageNr(start)) * pageSize;
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) + PAGESEL_RD + vid] = mem + (p - pageNr(start)) * pageSize;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) + PAGESEL_WR + vid] = bus.extSel.mask;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) + PAGESEL_RD + vid] = bus.data.mask | bus.extSel.mask;
        }
    }
}

// Map reads, block write accesses from modifying native RAM 
IRAM_ATTR void mmuMapRangeRO(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_WR | vid] = &dummyRam[0];
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = mem + (p - pageNr(start)) * pageSize;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_WR | vid] = bus.extSel.mask;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = bus.data.mask | bus.extSel.mask;
        }
    }
}

// Unmap range, let accesses fall through to native RAM 
IRAM_ATTR void mmuUnmapRange(uint16_t start, uint16_t end) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) {
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_WR | vid] = &dummyRam[0];
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = &dummyRam[0];
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_WR | vid] = 0;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = 0;
        }
    }
}

// Restore any original mapping to local SRAM if it existed.  Allow write accesses to also fall through and write to native RAM 
IRAM_ATTR void mmuRemapBaseRam(uint16_t start, uint16_t end) {
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            //L1Block *b = l1Blocks[page2L1Block(p) + PAGESEL_WR + vid];
            if (baseMemPages[p] != NULL) { 
                l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_WR | vid] = baseMemPages[p];
                l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = baseMemPages[p];
                l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_WR | vid] = 0;
                l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = bus.data.mask | bus.extSel.mask;
            } else { 
                l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_WR | vid] = &dummyRam[0];
                l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = &dummyRam[0];
                l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_WR | vid] = 0;
                l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = 0;
            }
        }
    }
}

// Called any time values in portb(0xd301) or newport(0xd1ff) change
IRAM_ATTR void mmuOnChange(bool force /*= false*/) {
    uint32_t stsc = XTHAL_GET_CCOUNT();
    mmuChangeBmonMaxStart = max((bmonHead - bmonTail) & bmonArraySzMask, mmuChangeBmonMaxStart); 
    uint8_t newport = d000Write[_0x1ff];
    uint8_t portb = d000Write[_0x301]; 

    DRAM_ATTR static bool lastBasicEn = true;
    DRAM_ATTR static bool lastPbiEn = false;
    DRAM_ATTR static bool lastPostEn = false;
    DRAM_ATTR static bool lastOsEn = true;
    DRAM_ATTR static int lastXeBankNr = 0;
    //DRAM_ATTR static int lastBankA0 = -1, lastBank80 = -1;

    // Extended memory bank switching.  This is mostly handled by core1.cpp, but if 
    // we're using PSRAM backed extended memory, we need to update the page tables here
    // while the 6502 is halted
    bool postEn = (portb & portbMask.selfTestEn) == 0;
    int xeBankNr = (portb & 0x7c) >> 2; 
    if (mmuState.extBanks[xeBankNr] != NULL && (lastXeBankNr != xeBankNr || force)) { 
        uint8_t *mem = extMem.getBank(xeBankNr);
        uint8_t *currentMappedMem = mmuState.extBanks[xeBankNr]->pages[pageNr(0) | PAGESEL_RD];
        // Did the memory change due to getBank() swapping it in from PSRAM?  If so, update page tables
        if (mem != NULL && mem != currentMappedMem) { 
            assert(config.haltAvailable == true); // we can only copy in PSRAM with 6502 halted
            for(int p = 0; p < pageNr(0x4000); p++) {  
                for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
                    mmuState.extBanks[xeBankNr]->pages[p | vid | PAGESEL_RD] = mem + p * pageSize;
                    mmuState.extBanks[xeBankNr]->pages[p | vid | PAGESEL_WR] = mem + p * pageSize;
                    //mmuState.l1Blocks[l1Block40] = mmuState.extBanks[xeBankNr];
                }
            }
        }
        lastXeBankNr = xeBankNr;
    }
    if (lastPostEn != postEn || force) { 
        if (postEn) {
            mmuUnmapRange(_0x5000, _0x57ff);
        } else { 
            mmuRemapBaseRam(_0x5000, _0x57ff);
        }
        lastPostEn = postEn;
    }
}

// verify the a8 address range is mapped to internal esp32 ram and is continuous 
// TODO: this only handles addresses mapped into L1 block base RAM, not extMem or other banks

IRAM_ATTR uint8_t *mmuCheckRangeMapped(MmuState &ms, uint16_t addr, uint16_t len) { 
#ifdef CSIM
    // Why is this needed, we tried to map it
    return &atariRam[addr];
#endif
    for(int p = pageNr(addr); p <= pageNr(addr + len - 1); p++) { 
        if (ms.l1Blocks[page2L1Block(p)]->ctrl[(p & pageInL1BlockMask) | PAGESEL_RD | PAGESEL_CPU] == 0)
            return NULL;
        if (ms.l1Blocks[page2L1Block(p)]->pages[(p & pageInL1BlockMask) | PAGESEL_WR | PAGESEL_CPU] == &dummyRam[0])
            return NULL;
        // check mapping is continuous 
        uint8_t *firstPageMem = ms.l1Blocks[page2L1Block(pageNr(addr))]->pages[(pageNr(addr) & pageInL1BlockMask) + PAGESEL_WR + PAGESEL_CPU];
        int offset = (p - pageNr(addr)) * pageSize;
        if (ms.l1Blocks[page2L1Block(p)]->pages[(p & pageInL1BlockMask) | PAGESEL_WR | PAGESEL_CPU] != firstPageMem + offset)
            return NULL;
    }
    return ms.l1Blocks[page2L1Block(pageNr(addr))]->pages[(pageNr(addr) & pageInL1BlockMask) | PAGESEL_WR | PAGESEL_CPU] + (addr & pageOffsetMask);
}

IRAM_ATTR void mmuInit() { 
    for(int b = 0; b < nrL1Blocks; b++)
        mmuState.l1Blocks[b] = &l1Blocks[b];

    mmuUnmapRange(0x0000, 0xffff);
    mmuAddBaseRam(0x0000, baseMemSz - 1, atariRam);
    //if (baseMemSz < 0x8000) 
    //    mmuAllocAddBaseRam(0x4000, 0x7fff);
    if (baseMemSz < 0xe000) 
        mmuAllocAddBaseRam(0xd800, 0xdfff);
    mmuUnmapRange(0xd000, 0xd7ff);

    //TODO: allow 130xe native ram to show through
    //mmuAddBaseRam(0x4000, 0x7fff, NULL);

    // map register writes for d000-d7ff to shadow write pages
    for(int p = pageNr(0xd000); p <= pageNr(0xd7ff); p++) { 
        l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_WR | PAGESEL_CPU] = &d000Write[0] + (p - pageNr(0xd000)) * pageSize;
    }
    
#if pageSize <= 0x100    
    // enable reads from 0xd500-0xd5ff for emulating RTC-8 and other cartsel features 
    for(int p = pageNr(0xd500); p <= pageNr(0xd5ff); p++) { 
        l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD] = &d000Read[(p - pageNr(0xd000)) * pageSize];
        l1Blocks[page2L1Block(p)].ctrl [(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
        l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_WR] = &d000Write[(p - pageNr(0xd000)) * pageSize];
        l1Blocks[page2L1Block(p)].ctrl [(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_WR] = bus.extSel.mask;
    }

    // Map register reads for the page containing 0xd1ff so we can handle reads to newport/0xd1ff 
    // implementing PBI interrupt scheme 
    l1Blocks[page2L1Block(pageNr(0xd1ff))].pages[(pageNr(0xd1ff) & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD] = &d000Read[(pageNr(0xd1ff) - pageNr(0xd000)) * pageSize];
    l1Blocks[page2L1Block(pageNr(0xd1ff))].ctrl[(pageNr(0xd1ff) & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
    
    // technically should support cartctl reads also
    // pageEnable[pageNr(0xd500) | PAGESEL_CPU | PAGESEL_RD ] |= pins.halt.mask;
#endif

    if (1 && config.haltAvailable) {
        // enable the halt(ready) line in response to writes to 0xd301, 0xd1ff or 0xd500
        //l1Blocks[page2L1Block(pageNr(0xd1ff))].ctrl[(pageNr(0xd1ff) & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_WR] |= bus.halt_.mask;
        l1Blocks[page2L1Block(pageNr(0xd301))].ctrl[(pageNr(0xd301) & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_WR] |= bus.halt_.mask;
        //l1Blocks[page2L1Block(pageNr(0xd500))].ctrl[(pageNr(0xd500) & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_WR] |= bus.halt_.mask;
    }

    // Intialize register shadow write memory to the default hardware reset values
    d000Write[0x301] = 0xff;
    d000Write[0x1ff] = 0x00;
    d000Read[0x1ff] = 0x00;
    mmuOnChange(true/*force*/);

    mmuRemapBaseRam(_0xc000, _0xcfff);
    mmuRemapBaseRam(_0xd800, _0xffff);
    osRomDisabledL1Block = l1Blocks[page2L1Block(pageNr(0xc000))];

    mmuUnmapRange(_0xc000, _0xcfff);
    mmuUnmapRange(_0xd800, _0xffff);
    osRomEnabledL1Block = l1Blocks[page2L1Block(pageNr(0xc000))];
    
    mmuMapRangeRWIsolated(_0xd800, _0xdfff, &pbiROM[0]);
    osRomEnabledPbiL1Block = l1Blocks[page2L1Block(pageNr(0xc000))];
    for(int a = 0; a < ARRAYSZ(osRomEnabledPbiL1Block.ctrl); a++)
        osRomEnabledPbiL1Block.ctrl[a] |= bus.mpd.mask;

    mmuState.osEnL1BlockMux[0] = &osRomDisabledL1Block;
    mmuState.osEnL1BlockMux[1] = &osRomEnabledL1Block;
    mmuState.osEnL1BlockMux[2] = &osRomEnabledPbiL1Block;
    mmuState.osEnL1BlockMux[3] = &osRomEnabledPbiL1Block;
#if 0 
    mmuState.osEnL1BlockMux[0] = &osRomEnabledL1Block;
#endif
    mmuUnmapRange(_0xd800, _0xdfff);
    mmuUnmapRange(_0xa000, 0xbfff);
    basicEnabledL1Block = l1Blocks[page2L1Block(pageNr(0x8000))];
    mmuState.basicEnL1BlockMux[0] = &basicEnabledL1Block;;

    mmuRemapBaseRam(_0xa000, 0xbfff);
    atariCart.initMmuBlocks();
    mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[0];
#if 0 
    mmuState.basicEnL1BlockMux[0] = mmuState.cartBanks[0];
#endif
    mmuOnChange(true/*force*/);

    // Allocate as many L1Blocks as needed to support mmuState.extBanks.  Valid extBank/PORTB select bit patterns
    // have a newly allocated L1Block describing that mapping, while unused bit patterns just point to the noramal
    // l1Blocks entry
    for(int i = 0; i < ARRAYSZ(mmuState.extBanks); i++) { 
        int extBank = extMem.premap[i];
        if (extBank >= 0) { // map in extended memory
            mmuState.extBanks[i] = static_cast<L1Block *>(heap_caps_malloc(sizeof(L1Block), MALLOC_CAP_INTERNAL));
            assert(mmuState.extBanks[i] != NULL);
            *mmuState.extBanks[i] = l1Blocks[page2L1Block(pageNr(0x4000))];

            for (int p = 0; p < pagesPerL1Block; p++) {
                mmuState.extBanks[i]->pages[p | PAGESEL_CPU | PAGESEL_RD] = extMem.banks[extMem.premap[i]] + p * pageSize;
                mmuState.extBanks[i]->pages[p | PAGESEL_CPU | PAGESEL_WR] = extMem.banks[extMem.premap[i]] + p * pageSize;
                mmuState.extBanks[i]->ctrl [p | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
                mmuState.extBanks[i]->ctrl [p | PAGESEL_CPU | PAGESEL_WR] = bus.extSel.mask;
            }
        } else { // no ext mem for this PORTB bit pattern, map in existing base ram 
            mmuState.extBanks[i] = &l1Blocks[page2L1Block(pageNr(0x4000))];
        }
    }
    mmuState.l1Blocks[l1Block40]= mmuState.extBanks[(d000Write[_0x301] & 0x7c) >> 2];

    // initialize mmuStateDisabled and mmuStateSaved
    for(auto &p : dummyL1Block.pages) p = dummyRam;
    for(auto &c : dummyL1Block.ctrl) c = 0;
    for(auto &b : mmuStateDisabled.l1Blocks) b = &dummyL1Block;
    for(auto &b : mmuStateDisabled.basicEnL1BlockMux) b = &dummyL1Block;
    for(auto &b : mmuStateDisabled.osEnL1BlockMux) b = &dummyL1Block;
    for(auto &b : mmuStateDisabled.cartBanks) b = &dummyL1Block;
    for(auto &b : mmuStateDisabled.extBanks) b = &dummyL1Block;
    mmuStateSaved = mmuState;
}

void mmuDebugPrint() { mmuDebugPrintMmuState(mmuState); } 

void mmuDebugPrintMmuState(MmuState &ms) {
    uint8_t *lastMem = 0; 
    int seqCount = 0;
    for(int p = 0; p < nrPages; p++) { 
        uint8_t *mem = ms.l1Blocks[page2L1Block(p)]->pages[(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD];
        string what = "(unknown)";
        if (mem >= atariRam && mem < atariRam + baseMemSz) what = sfmt("(basemem at %p)", atariRam);
        for(int b = 0; b < atariCart.bankCount; b++) {
            if (mem >= atariCart.image[b].mem && mem < atariCart.image[b].mem + 0x2000) what = sfmt("(cart bank %d at %p)", b, atariCart.image[b].mem);
        }
	    if (mem >= dummyRam && mem < dummyRam + pageSize) what = "(dummy ram)";
	    if (mem >= pbiROM && mem < pbiROM + sizeof(pbiROM)) what = "(PBI rom)";
	    if (mem >= d000Read && mem < d000Read + sizeof(d000Read)) what = sfmt("(d000 read page %d)", 
			(mem - d000Read) / pageSize);
        if (mem == NULL) what = "(unmapped)";
        if (mem != lastMem + pageSize && mem != lastMem) {
            if (seqCount > 0) printf("...\n");
            seqCount = 0;
            printf("%04x   %p   ", p * pageSize, mem);
            printf("%s\n", what.c_str());
        } else {
            seqCount++;
        }
        lastMem = mem;
    }
#if 0 
    // print extmem
    for(int i = 0; i < ARRAYSZ(ms.extBanks); i++) {
        printf("extbank %02d: ", i); 
        if (ms.extBanks[i] == &l1Blocks[page2L1Block(pageNr(0x4000))]) {
            printf("(basemem 0x4000)\n");
            continue;
        }  
        bool found = false;
        for(int b = 0; b < ARRAYSZ(extMem.premap); b++) { 
            if (extMem.premap[b] >= 0) { 
                if (ms.extBanks[i]->pages[0 | PAGESEL_CPU | PAGESEL_RD] == extMem.banks[extMem.premap[b]]) { 
                    printf("(extmem bank %d)", b);
                    found = true;
                } else if (extMem.banks[extMem.premap[b]] == extMem.spare) {
                    printf("(extmem spare bank)");
                    found = true;
                }
            }
        }
        if (!found) {
            printf("(unknown)");
        }
        if (ms.extBanks[i] == ms.l1Blocks[l1Block40]) printf ("(active)");
        printf("\n");
    }
#endif
}
