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

DRAM_ATTR uint8_t *baseMemPages[nrPages] = {0};
DRAM_ATTR uint8_t atariRam[baseMemSz] = {0x0};
DRAM_ATTR uint8_t dummyRam[pageSize] = {0x0};
DRAM_ATTR uint8_t d000Write[0x800] = {0x0};
DRAM_ATTR uint8_t d000Read[0x800] = {0xff};

DRAM_ATTR uint8_t *screenMem = NULL;
DRAM_ATTR uint8_t pbiROM[0x800] = {
#include "pbirom.h"
};
//BankL1Entry are 1.5K each, this is about 64K of memory :(  
DRAM_ATTR BankL1Entry banksL1[nrL1Banks] = {0}; // 6K
DRAM_ATTR BankL1Entry basicEnabledBank, basicDisabledBank, osRomEnabledBank, osRomEnabledBankPbiEn, osRomDisabledBank, dummyBank; // 9K
//DRAM_ATTR BankL1Entry extMemBanks[32]; // 48K

//static constexpr int x = sizeof(banksL1);
DRAM_ATTR RAM_VOLATILE MmuState mmuState;
DRAM_ATTR RAM_VOLATILE MmuState mmuStateSaved;
DRAM_ATTR RAM_VOLATILE MmuState mmuStateDisabled;

DRAM_ATTR BUSCTL_VOLATILE uint32_t pinReleaseMask = bus.irq_.mask | bus.data.mask | bus.extSel.mask | bus.mpd.mask | bus.halt_.mask;

//DRAM_ATTR RAM_VOLATILE BankL1Entry *basicEnBankMux[2] = {0};
//DRAM_ATTR RAM_VOLATILE BankL1Entry *osEnBankMux[4] = {0};

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
            banksL1[page2bank(p)].pages[(p & pageInBankMask) + PAGESEL_WR + vid] = mem + (p - pageNr(start)) * pageSize;
            banksL1[page2bank(p)].pages[(p & pageInBankMask) + PAGESEL_RD + vid] = mem + (p - pageNr(start)) * pageSize;
            banksL1[page2bank(p)].ctrl[(p & pageInBankMask) + PAGESEL_WR + vid] = 0;
            banksL1[page2bank(p)].ctrl[(p & pageInBankMask) + PAGESEL_RD + vid] = bus.data.mask | bus.extSel.mask;
        }
    }
}

// Map reads and writes to local SRAM, but block write accesses from modifying native RAM 
IRAM_ATTR void mmuMapRangeRWIsolated(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            banksL1[page2bank(p)].pages[(p & pageInBankMask) + PAGESEL_WR + vid] = mem + (p - pageNr(start)) * pageSize;
            banksL1[page2bank(p)].pages[(p & pageInBankMask) + PAGESEL_RD + vid] = mem + (p - pageNr(start)) * pageSize;
            banksL1[page2bank(p)].ctrl[(p & pageInBankMask) + PAGESEL_WR + vid] = bus.extSel.mask;
            banksL1[page2bank(p)].ctrl[(p & pageInBankMask) + PAGESEL_RD + vid] = bus.data.mask | bus.extSel.mask;
        }
    }
}

// Map reads, block write accesses from modifying native RAM 
IRAM_ATTR void mmuMapRangeRO(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_WR | vid] = &dummyRam[0];
            banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_RD | vid] = mem + (p - pageNr(start)) * pageSize;
            banksL1[page2bank(p)].ctrl[(p & pageInBankMask) | PAGESEL_WR | vid] = bus.extSel.mask; 
            banksL1[page2bank(p)].ctrl[(p & pageInBankMask) | PAGESEL_RD | vid] = bus.data.mask | bus.extSel.mask;
        }
    }
}

// Unmap range, let accesses fall through to native RAM 
IRAM_ATTR void mmuUnmapRange(uint16_t start, uint16_t end) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) {
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_WR | vid] = &dummyRam[0];
            banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_RD | vid] = &dummyRam[0];
            banksL1[page2bank(p)].ctrl[(p & pageInBankMask) | PAGESEL_WR | vid] = 0; 
            banksL1[page2bank(p)].ctrl[(p & pageInBankMask) | PAGESEL_RD | vid] = 0;
        }
    }
}

// Restore any original mapping to local SRAM if it existed.  Allow write accesses to also fall through and write to native RAM 
IRAM_ATTR void mmuRemapBaseRam(uint16_t start, uint16_t end) {
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            //BankL1Entry *b = banksL1[bankToPage(p) + PAGESEL_WR + vid];
            if (baseMemPages[p] != NULL) { 
                banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_WR | vid] = baseMemPages[p];
                banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_RD | vid] = baseMemPages[p];
                banksL1[page2bank(p)].ctrl[(p & pageInBankMask) | PAGESEL_WR | vid] = 0;
                banksL1[page2bank(p)].ctrl[(p & pageInBankMask) | PAGESEL_RD | vid] = bus.data.mask | bus.extSel.mask;
            } else { 
                banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_WR | vid] = &dummyRam[0];
                banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_RD | vid] = &dummyRam[0];
                banksL1[page2bank(p)].ctrl[(p & pageInBankMask) | PAGESEL_WR | vid] = 0;
                banksL1[page2bank(p)].ctrl[(p & pageInBankMask) | PAGESEL_RD | vid] = 0;
            }
        }
    }
}

#if 0 
IRAM_ATTR void mmuMapPbiRom(bool pbiEn, bool osEn) {
    if (pbiEn) {
        pinReleaseMask &= bus.mpd.maskInverse;
        pinDriveMask |= bus.mpd.mask;
    } else {
        pinReleaseMask |= bus.mpd.mask;
        pinDriveMask &= bus.mpd.maskInverse;
    }
}
#endif 

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

#if 0
    bool osEn = (portb & portbMask.osEn) != 0;
    bool pbiEn = (newport & pbiDeviceNumMask) != 0;
    static constexpr DRAM_ATTR int bankC0 = page2bank(pageNr(0xc000)); // bank to remap for cart control 
    if (pbiEn != lastPbiEn || force) {
        mmuMapPbiRom(pbiEn, osEn);
        lastPbiEn = pbiEn;
    }
    //osEnBankMux[1] = pbiEn ? &osRomEnabledBankPbiEn : &osRomEnabledBank;
    //banks[bankC0] = osEnBankMux[osEn];
#endif

    // Figured this out - the native ram under the bank window can't be used usable during
    // bank switching becuase it catches the writes to extended ram and gets corrupted. 
    // Once a sparse base memory map is implemented, we will need to leave this 16K
    // mapped to emulated RAM.  

#if 1
    bool postEn = (portb & portbMask.selfTestEn) == 0;
    int xeBankNr = (portb & 0x7c) >> 2; 
    if (mmuState.extBanks[xeBankNr] != NULL && (lastXeBankNr != xeBankNr || force)) { 
        uint8_t *mem = extMem.getBank(xeBankNr);
        uint8_t *currentMappedMem = mmuState.extBanks[xeBankNr]->pages[pageNr(0) | PAGESEL_RD];
        // Did the memory change due to getBank() swapping it in from PSRAM?  If so, update page tables 
        if (mem != NULL && mem != currentMappedMem) { 
            for(int p = 0; p < pageNr(0x4000); p++) {  
                for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
                    mmuState.extBanks[xeBankNr]->pages[p | vid | PAGESEL_RD] = mem + p * pageSize;
                    mmuState.extBanks[xeBankNr]->pages[p | vid | PAGESEL_WR] = mem + p * pageSize;
                }
            }
        }
        lastXeBankNr = xeBankNr;
    }
#endif 

#if 0 
    if (lastXeBankNr != xeBankNr || force) { 
        uint8_t *mem;
        if ((mem = extMem.getBank(xeBankNr)) != NULL) { 
            mmuMapRangeRWIsolated(_0x4000, _0x7fff, mem);
        } else { 
            mmuRemapBaseRam(_0x4000, _0x7fff);
        }
        if (postEn) 
            mmuUnmapRange(_0x5000, _0x57ff);
        lastXeBankNr = xeBankNr;
    }
    if (lastPostEn != postEn || force) { 
        uint8_t *mem;
        if (postEn) {
            mmuUnmapRange(_0x5000, _0x57ff);
        } else if ((mem = extMem.getBank(xeBankNr)) != NULL) { 
            mmuMapRangeRWIsolated(_0x4000, _0x7fff, mem);
        } else { 
            mmuRemapBaseRam(_0x5000, _0x57ff);
        }
        lastPostEn = postEn;
    }
#endif

    bool basicEn = (portb & portbMask.basicEn) == 0;
#if 0 
    if (lastBasicEn != basicEn || lastBankA0 != atariCart.bankA0 || force) { 
        static constexpr DRAM_ATTR int bank80 = page2bank(pageNr(0x8000)); // bank to remap for cart control 
        if (basicEn) { 
            banks[bank80] = &basicEnabledBank;
        } else { 
            banks[bank80] = cartBanks[lastPageOffset[pageNr(_0xd500)] & 0x1f];
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
#endif
}

// verify the a8 address range is mapped to internal esp32 ram and is continuous 
IRAM_ATTR uint8_t *mmuCheckRangeMapped(uint16_t addr, uint16_t len) { 
    for(int p = pageNr(addr); p <= pageNr(addr + len - 1); p++) { 
        if (banksL1[page2bank(p)].ctrl[(p & pageInBankMask) | PAGESEL_RD | PAGESEL_CPU] == 0) 
            return NULL;
        if (banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_WR | PAGESEL_CPU] == &dummyRam[0]) 
            return NULL;
        // check mapping is continuous 
        uint8_t *firstPageMem = banksL1[page2bank(pageNr(addr))].pages[(pageNr(addr) & pageInBankMask) + PAGESEL_WR + PAGESEL_CPU];
        int offset = (p - pageNr(addr)) * pageSize;
        if (banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_WR | PAGESEL_CPU] != firstPageMem + offset)
            return NULL;
    }
    return banksL1[page2bank(pageNr(addr))].pages[(pageNr(addr) & pageInBankMask) | PAGESEL_WR | PAGESEL_CPU] + (addr & pageOffsetMask);
}

IRAM_ATTR void mmuInit() { 
    for(int b = 0; b < nrL1Banks; b++) 
        mmuState.banks[b] = &banksL1[b];

    mmuUnmapRange(0x0000, 0xffff);
    mmuAddBaseRam(0x0000, baseMemSz - 1, atariRam);
    if (baseMemSz < 0x8000) 
        mmuAllocAddBaseRam(0x4000, 0x7fff);
    if (baseMemSz < 0xe000) 
        mmuAllocAddBaseRam(0xd800, 0xdfff);
    mmuUnmapRange(0xd000, 0xd7ff);

    //TODO: allow 130xe native ram to show through
    mmuAddBaseRam(0x4000, 0x7fff, NULL);

    // map register writes for d000-d7ff to shadow write pages
    for(int p = pageNr(0xd000); p <= pageNr(0xd7ff); p++) { 
        banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_WR | PAGESEL_CPU] = &d000Write[0] + (p - pageNr(0xd000)) * pageSize; 
    }
    
#if pageSize <= 0x100    
    // enable reads from 0xd500-0xd5ff for emulating RTC-8 and other cartsel features 
    for(int p = pageNr(0xd500); p <= pageNr(0xd5ff); p++) { 
        banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_CPU | PAGESEL_RD] = &d000Read[(p - pageNr(0xd000)) * pageSize]; 
        banksL1[page2bank(p)].ctrl [(p & pageInBankMask) | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
        banksL1[page2bank(p)].pages[(p & pageInBankMask) | PAGESEL_CPU | PAGESEL_WR] = &d000Write[(p - pageNr(0xd000)) * pageSize]; 
        banksL1[page2bank(p)].ctrl [(p & pageInBankMask) | PAGESEL_CPU | PAGESEL_WR] = bus.extSel.mask;
    }

    // Map register reads for the page containing 0xd1ff so we can handle reads to newport/0xd1ff 
    // implementing PBI interrupt scheme 
    banksL1[page2bank(pageNr(0xd1ff))].pages[(pageNr(0xd1ff) & pageInBankMask) | PAGESEL_CPU | PAGESEL_RD] = &d000Read[(pageNr(0xd1ff) - pageNr(0xd000)) * pageSize]; 
    banksL1[page2bank(pageNr(0xd1ff))].ctrl[(pageNr(0xd1ff) & pageInBankMask) | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
    
    // technically should support cartctl reads also
    // pageEnable[pageNr(0xd500) | PAGESEL_CPU | PAGESEL_RD ] |= pins.halt.mask;
#endif

    // enable the halt(ready) line in response to writes to 0xd301, 0xd1ff or 0xd500
    //banksL1[page2bank(pageNr(0xd1ff))].ctrl[(pageNr(0xd1ff) & pageInBankMask) | PAGESEL_CPU | PAGESEL_WR] |= bus.halt_.mask;
    //banksL1[page2bank(pageNr(0xd301))].ctrl[(pageNr(0xd301) & pageInBankMask) | PAGESEL_CPU | PAGESEL_WR] |= bus.halt_.mask;
    //banksL1[page2bank(pageNr(0xd500))].ctrl[(pageNr(0xd500) & pageInBankMask) | PAGESEL_CPU | PAGESEL_WR] |= bus.halt_.mask;

    // Intialize register shadow write memory to the default hardware reset values
    d000Write[0x301] = 0xff;
    d000Write[0x1ff] = 0x00;
    d000Read[0x1ff] = 0x00;
    mmuOnChange(true/*force*/);

    mmuRemapBaseRam(_0xc000, _0xcfff);
    mmuRemapBaseRam(_0xd800, _0xffff);
    osRomDisabledBank = banksL1[page2bank(pageNr(0xc000))];

    mmuUnmapRange(_0xc000, _0xcfff);
    mmuUnmapRange(_0xd800, _0xffff);
    osRomEnabledBank = banksL1[page2bank(pageNr(0xc000))];
    
    mmuMapRangeRWIsolated(_0xd800, _0xdfff, &pbiROM[0]);
    osRomEnabledBankPbiEn = banksL1[page2bank(pageNr(0xc000))];
    for(int a = 0; a < ARRAYSZ(osRomEnabledBankPbiEn.ctrl); a++) 
        osRomEnabledBankPbiEn.ctrl[a] |= bus.mpd.mask;

    mmuState.osEnBankMux[0] = &osRomDisabledBank; 
    mmuState.osEnBankMux[1] = &osRomEnabledBank;
    mmuState.osEnBankMux[2] = &osRomEnabledBankPbiEn;
    mmuState.osEnBankMux[3] = &osRomEnabledBankPbiEn;
#if 0 
    mmuState.osEnBankMux[0] = &osRomEnabledBank;
#endif
    mmuUnmapRange(_0xd800, _0xdfff);
    mmuUnmapRange(_0xa000, 0xbfff);
    basicEnabledBank = banksL1[page2bank(pageNr(0x8000))];
    mmuState.basicEnBankMux[0] = &basicEnabledBank;;

    mmuRemapBaseRam(_0xa000, 0xbfff);
    atariCart.initMmuBank();
    mmuState.basicEnBankMux[1] = mmuState.cartBanks[0];
#if 0 
    mmuState.basicEnBankMux[0] = mmuState.cartBanks[0];
#endif
    mmuOnChange(true/*force*/);

    // Allocate as many BankL1Entrys as needed to support mmuState.extBanks.  Valid extBank/PORTB select bit patterns
    // have a newly allocated BankL1Entry describing that mapping, while unused bit patterns just point to the noramal
    // banksL1 entry 
    for(int i = 0; i < ARRAYSZ(mmuState.extBanks); i++) { 
        int extBank = extMem.premap[i];
        if (extBank >= 0) { // map in extended memory
            mmuState.extBanks[i] = static_cast<BankL1Entry *>(heap_caps_malloc(sizeof(BankL1Entry), MALLOC_CAP_INTERNAL));
            assert(mmuState.extBanks[i] != NULL);
            *mmuState.extBanks[i] = banksL1[page2bank(pageNr(0x4000))];

            for (int p = 0; p < pagesPerBank; p++) { 
                mmuState.extBanks[i]->pages[p | PAGESEL_CPU | PAGESEL_RD] = extMem.banks[extMem.premap[i]] + p * pageSize;
                mmuState.extBanks[i]->pages[p | PAGESEL_CPU | PAGESEL_WR] = extMem.banks[extMem.premap[i]] + p * pageSize;
                mmuState.extBanks[i]->ctrl [p | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
                mmuState.extBanks[i]->ctrl [p | PAGESEL_CPU | PAGESEL_WR] = 0;
            }
        } else { // no ext mem for this PORTB bit pattern, map in existing base ram 
            mmuState.extBanks[i] = &banksL1[page2bank(pageNr(0x4000))];
        }
    }

    // initialize mmuStateDisabled and mmuStateSaved
    for(auto &p : dummyBank.pages) p = dummyRam;
    for(auto &c : dummyBank.ctrl) c = 0;
    for(auto &b : mmuStateDisabled.banks) b = &dummyBank;
    for(auto &b : mmuStateDisabled.basicEnBankMux) b = &dummyBank;
    for(auto &b : mmuStateDisabled.osEnBankMux) b = &dummyBank;
    for(auto &b : mmuStateDisabled.cartBanks) b = &dummyBank;
    for(auto &b : mmuStateDisabled.extBanks) b = &dummyBank;
    mmuStateSaved = mmuState;
}


void mmuDebugPrint() {
    uint8_t *lastMem = 0; 
    int seqCount = 0;
    for(int p = 0; p < nrPages; p++) { 
        uint8_t *mem = mmuState.banks[page2bank(p)]->pages[(p & pageInBankMask) | PAGESEL_CPU | PAGESEL_RD];
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
}
