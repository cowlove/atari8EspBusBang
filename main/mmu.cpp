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
uint8_t d000Write[0x800];   // core 1 hot data: .bss (mmu.h)
uint8_t d000Read[0x800];

DRAM_ATTR uint8_t *screenMem = NULL;
// pbiROM[] ($D800-$DFFF) is core 1 hot data too: .bss, filled from
// pbiROMImage[] (flash rodata) by the static constructor below.
extern "C" uint8_t pbiROM[0x800];
uint8_t pbiROM[0x800];
static const uint8_t pbiROMImage[] = {
#include "pbirom.h"
};
static_assert(sizeof(pbiROMImage) <= sizeof(pbiROM), "PBI ROM image larger than pbiROM[]");
// L1 blocks are 1.5K each; four entries use about 6K.
L1Block l1Blocks[nrL1Blocks]; // 6K, core 1 hot data: .bss (mmu.h)
L1Block basicEnabledL1Block, basicDisabledL1Block, osRomEnabledL1Block, osRomEnabledPbiL1Block, osRomDisabledL1Block, dummyL1BlockWithHalt, dummyL1Block; // 9K
//DRAM_ATTR L1Block extMemBanks[32]; // 48K

//static constexpr int x = sizeof(l1Blocks);
RAM_VOLATILE MmuState mmuState;   // core 1 hot data: .bss (mmu.h)
DRAM_ATTR RAM_VOLATILE MmuState mmuStateSaved;
DRAM_ATTR RAM_VOLATILE MmuState mmuStateDisabled;

// The core 1 hot data's initial values, which .bss can't carry: run at
// program start (before app_main() on the ESP32; before csim's atari800
// copies pbiROM[] at the 6502's first $D1FF write).
static struct MmuStaticInit {
    MmuStaticInit() {
        memcpy(pbiROM, pbiROMImage, sizeof(pbiROMImage));
        mmuState.pinReleaseMask = kPinReleaseMaskInit;
        d000Read[0] = 0xff;
    }
} mmuStaticInit;
DRAM_ATTR int baseMemSz = 2 * 1024;

// pinReleaseMask: now mmuState.pinReleaseMask (mmu.h).

//DRAM_ATTR RAM_VOLATILE L1Block *basicEnL1BlockMux[2] = {};
//DRAM_ATTR RAM_VOLATILE L1Block *osEnL1BlockMux[4] = {};

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

// Base RAM allocated in pieces, with each page's owner recorded, so a
// range can be torn down again (mmuFreeUnmapBaseRam()), e.g. when memory
// autodetection shrinks the 64K boot map (docs/memory-autodetect.md).
// Pieces are at most baseRamPieceMax: IDF's heap is good-fit, so they fill
// small free blocks and holes first and leave the large contiguous blocks
// (16K extended-bank buffers, WiFi) alone. Each piece is contiguous, so
// SIO's direct path (mmuCheckRangeMapped()) works unless a transfer
// straddles two pieces.
static constexpr int baseRamPieceMax = 4096;
static constexpr int maxBaseRamPieces = 64;
struct BaseRamPiece { uint8_t *mem; uint16_t firstPage, pages, live; };
static DRAM_ATTR BaseRamPiece baseRamPieces[maxBaseRamPieces];
static DRAM_ATTR uint8_t basePagePiece[nrPages];   // piece index + 1, 0: not ours

uint8_t *mmuAllocAddBaseRam(uint16_t start, uint16_t end) {
    uint8_t *first = nullptr;
    for (int p = pageNr(start), last = pageNr(end); p <= last; ) {
        int want = last - p + 1;
        if (want > baseRamPieceMax / pageSize) want = baseRamPieceMax / pageSize;
        uint8_t *mem = nullptr;
        while ((mem = (uint8_t *)heap_caps_malloc(want * pageSize, MALLOC_CAP_INTERNAL)) == nullptr && want > 1)
            want /= 2;
        assert(mem != nullptr);
        int slot = 0;
        while (slot < maxBaseRamPieces && baseRamPieces[slot].mem != nullptr) slot++;
        assert(slot < maxBaseRamPieces);
        baseRamPieces[slot] = {mem, uint16_t(p), uint16_t(want), uint16_t(want)};
        for (int i = 0; i < want; i++) basePagePiece[p + i] = slot + 1;
        bzero(mem, want * pageSize);
        mmuAddBaseRam(p * pageSize, (p + want) * pageSize - 1, mem);
        if (first == nullptr) first = mem;
        p += want;
    }
    return first;
}

// Every L1Block the MMU can reach: the static ones, and whatever the live
// and saved MmuState tables point at (copies, cartridge and bank blocks).
void mmuForEachL1Block(void (*fn)(L1Block *block, void *ctx), void *ctx) {
    static L1Block *seen[160];
    int n = 0;
    auto visit = [&](L1Block *b) {
        if (b == nullptr) return;
        for (int i = 0; i < n; i++) if (seen[i] == b) return;
        if (n < (int)ARRAYSZ(seen)) seen[n++] = b;
        fn(b, ctx);
    };
    for (auto &b : l1Blocks) visit((L1Block *)&b);
    visit((L1Block *)&basicEnabledL1Block); visit((L1Block *)&basicDisabledL1Block);
    visit((L1Block *)&osRomEnabledL1Block); visit((L1Block *)&osRomEnabledPbiL1Block);
    visit((L1Block *)&osRomDisabledL1Block);
    for (MmuState *ms : {(MmuState *)&mmuState, (MmuState *)&mmuStateSaved}) {
        for (L1Block *b : ms->l1Blocks) visit(b);
        for (L1Block *b : ms->basicEnL1BlockMux) visit(b);
        for (L1Block *b : ms->osEnL1BlockMux) visit(b);
        for (L1Block *b : ms->cartBanks) visit(b);
        for (L1Block *b : ms->extBanks) visit(b);
    }
}

// Unmap the base RAM this file allocated for [start, end], in every L1Block
// that points at it (native RAM answers there afterwards), and free each
// piece when its last page goes. Pages it didn't allocate (I/O, PBI ROM and
// RAM, the screen, a caller's own memory) are left alone. Cold: run with
// the bus detached.
void mmuFreeUnmapBaseRam(uint16_t start, uint16_t end) {
    struct Range { uint8_t *lo, *hi; } range;
    for (int p = pageNr(start); p <= pageNr(end); p++) {
        const int idx = basePagePiece[p];
        if (idx == 0) continue;
        BaseRamPiece &piece = baseRamPieces[idx - 1];
        range.lo = piece.mem + (p - piece.firstPage) * pageSize;
        range.hi = range.lo + pageSize;
        mmuForEachL1Block([](L1Block *b, void *ctx) {
            const Range &r = *(const Range *)ctx;
            for (int i = 0; i < (int)ARRAYSZ(b->pages); i++)
                if (b->pages[i] >= r.lo && b->pages[i] < r.hi) {
                    b->pages[i] = (uint8_t *)&dummyRam[0];
                    b->ctrl[i] = 0;
                }
        }, &range);
        baseMemPages[p] = NULL;
        basePagePiece[p] = 0;
        if (--piece.live == 0) {
            heap_caps_free(piece.mem);
            piece.mem = nullptr;
        }
    }
}

// Map reads and writes to local SRAM, but allow write accesses to also fall through and write to native RAM 
IRAM_ATTR void mmuMapRangeRW(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) + PAGESEL_WR + vid] = mem + (p - pageNr(start)) * pageSize;
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) + PAGESEL_RD + vid] = mem + (p - pageNr(start)) * pageSize;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) + PAGESEL_WR + vid] = 0;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) + PAGESEL_RD + vid] = bus.data.mask | bus.ctrlExtSel.mask;
        }
    }
}

// Map reads and writes to local SRAM, but block write accesses from modifying native RAM 
IRAM_ATTR void mmuMapRangeRWIsolated(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) + PAGESEL_WR + vid] = mem + (p - pageNr(start)) * pageSize;
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) + PAGESEL_RD + vid] = mem + (p - pageNr(start)) * pageSize;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) + PAGESEL_WR + vid] = bus.ctrlExtSel.mask;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) + PAGESEL_RD + vid] = bus.data.mask | bus.ctrlExtSel.mask;
        }
    }
}

// Map reads, block write accesses from modifying native RAM 
IRAM_ATTR void mmuMapRangeRO(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int p = pageNr(start); p <= pageNr(end); p++) { 
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_WR | vid] = &dummyRam[0];
            l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = mem + (p - pageNr(start)) * pageSize;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_WR | vid] = bus.ctrlExtSel.mask;
            l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = bus.data.mask | bus.ctrlExtSel.mask;
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
                l1Blocks[page2L1Block(p)].ctrl[(p & pageInL1BlockMask) | PAGESEL_RD | vid] = bus.data.mask | bus.ctrlExtSel.mask;
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
    // The memory probe (pbi.cpp) cycles PORTB through every value with the
    // banks unmapped; leave bank paging alone until it restores PORTB and
    // the map (it skips the self-test remap too, and PORTB ends as before).
    extern bool memProbeActive;
    if (memProbeActive) return;
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
    L1Block *xeBlock = mmuState.extBanks[portb];
    if (xeBlock != NULL && (lastXeBankNr != xeBankNr || force)) { 
        uint8_t *mem = extMem.getBank(xeBankNr);
        uint8_t *currentMappedMem = xeBlock->pages[pageNr(0) | PAGESEL_RD];
        // Did the memory change due to getBank() swapping it in from PSRAM?  If so, update page tables
        if (mem != NULL && mem != currentMappedMem) { 
            assert(config.haltAvailable == true); // we can only copy in PSRAM with 6502 halted
            for(int p = 0; p < pageNr(0x4000); p++) {  
                for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
                    xeBlock->pages[p | vid | PAGESEL_RD] = mem + p * pageSize;
                    xeBlock->pages[p | vid | PAGESEL_WR] = mem + p * pageSize;
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
// TODO: rethink the comment above?  if an extMem block is mapped, it should be in l1Blocks
//       thanks to core1.cpp manipulating the map 
IRAM_ATTR uint8_t *mmuCheckRangeMapped(MmuState &ms, uint16_t addr, uint16_t len) { 
    for(int p = pageNr(addr); p <= pageNr(addr + len - 1); p++) { 
        if (mmuBlock(ms, page2L1Block(p))->ctrl[(p & pageInL1BlockMask) | PAGESEL_RD | PAGESEL_CPU] == 0)
            return NULL;
        if (mmuBlock(ms, page2L1Block(p))->pages[(p & pageInL1BlockMask) | PAGESEL_WR | PAGESEL_CPU] == &dummyRam[0])
            return NULL;
        // check mapping is continuous 
        uint8_t *firstPageMem = mmuBlock(ms, page2L1Block(pageNr(addr)))->pages[(pageNr(addr) & pageInL1BlockMask) + PAGESEL_WR + PAGESEL_CPU];
        int offset = (p - pageNr(addr)) * pageSize;
        if (mmuBlock(ms, page2L1Block(p))->pages[(p & pageInL1BlockMask) | PAGESEL_WR | PAGESEL_CPU] != firstPageMem + offset)
            return NULL;
    }
    return mmuBlock(ms, page2L1Block(pageNr(addr)))->pages[(pageNr(addr) & pageInL1BlockMask) | PAGESEL_WR | PAGESEL_CPU] + (addr & pageOffsetMask);
}

// The extended-bank L1Blocks, from extMem.premap: valid extBank/PORTB
// select bit patterns get a newly allocated L1Block describing that
// mapping, unused ones point to the normal $4000 block. extBanks[] is
// indexed by the whole PORTB byte: every value with the same bank select
// bits (2-6) gets that bank's L1Block. Frees the previous build's blocks, so
// autoMemConfig() (pbi.cpp) can run it again after extMem.init(); with the
// bus detached, into mmuStateSaved.
void mmuBuildBankBlocks(MmuState &ms) {
    static L1Block *built[ARRAYSZ(extMem.premap)] = {};
    for (L1Block *&b : built) {
        if (b) heap_caps_free(b);
        b = nullptr;
    }
    L1Block *bankBlocks[ARRAYSZ(extMem.premap)];
    for(int i = 0; i < ARRAYSZ(bankBlocks); i++) { 
        int extBank = extMem.premap[i];
        if (extBank >= 0) { // map in extended memory
            bankBlocks[i] = static_cast<L1Block *>(heap_caps_malloc(sizeof(L1Block), MALLOC_CAP_INTERNAL));
            assert(bankBlocks[i] != NULL);
            built[i] = bankBlocks[i];
            *bankBlocks[i] = l1Blocks[page2L1Block(pageNr(0x4000))];

            for (int p = 0; p < pagesPerL1Block; p++) {
                bankBlocks[i]->pages[p | PAGESEL_CPU | PAGESEL_RD] = extMem.banks[extMem.premap[i]] + p * pageSize;
                bankBlocks[i]->pages[p | PAGESEL_CPU | PAGESEL_WR] = extMem.banks[extMem.premap[i]] + p * pageSize;
                bankBlocks[i]->ctrl [p | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.ctrlExtSel.mask;
                bankBlocks[i]->ctrl [p | PAGESEL_CPU | PAGESEL_WR] = bus.ctrlExtSel.mask;
            }
        } else { // no ext mem for this PORTB bit pattern, map in existing base ram 
            bankBlocks[i] = &l1Blocks[page2L1Block(pageNr(0x4000))];
        }
    }
    static_assert(ARRAYSZ(ms.extBanks) == 256 && ARRAYSZ(extMem.premap) == 32,
                  "extBanks[] is by PORTB byte, premap[] by its bits 2-6");
    for(int portb = 0; portb < ARRAYSZ(ms.extBanks); portb++)
        ms.extBanks[portb] = bankBlocks[(portb & 0x7c) >> 2];
    ms.l1Blocks[l1Block40] = ms.extBanks[d000Write[_0x301]];
}

// Halt the 6502 on PORTB writes (core 0 pages a PSRAM bank in, then
// resumes it). Only in the $C000 blocks, where $D301 is: l1Blocks[] and the
// OS-select copies (the same page offset in another block is $1300, $5300
// or $9300).
void mmuSetPortbHalt(bool on) {
    for (L1Block *b : {(L1Block *)&l1Blocks[page2L1Block(pageNr(0xd301))],
                       (L1Block *)&osRomEnabledL1Block, (L1Block *)&osRomEnabledPbiL1Block,
                       (L1Block *)&osRomDisabledL1Block}) {
        auto &c = b->ctrl[(pageNr(0xd301) & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_WR];
        c = on ? (c | bus.halt_.mask) : (c & bus.halt_.maskInverse);
    }
}

IRAM_ATTR void mmuInit() { 
    for(int b = 0; b < nrL1Blocks; b++)
        mmuState.l1Blocks[b] = &l1Blocks[b];

    mmuUnmapRange(0x0000, 0xffff);
    if (config.autoMem) {
        // AUTO memory: 64K for the OS's RAM sizing at power-on, as the 2K
        // atariRam plus pieces by range, so autoMemConfig() (pbi.cpp) can
        // free $0800-$D7FF and $E000-$FFFF after the probe and keep
        // $D800-$DFFF, which is what a 2K boot has.
        mmuAddBaseRam(0x0000, 0x07ff, atariRam);
        mmuAllocAddBaseRam(0x0800, 0xd7ff);
        mmuAllocAddBaseRam(0xd800, 0xdfff);
        mmuAllocAddBaseRam(0xe000, 0xffff);
    } else {
        mmuAddBaseRam(0x0000, baseMemSz - 1, atariRam);
        //if (baseMemSz < 0x8000) 
        //    mmuAllocAddBaseRam(0x4000, 0x7fff);
        if (baseMemSz < 0xe000) 
            mmuAllocAddBaseRam(0xd800, 0xdfff);
    }
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
        l1Blocks[page2L1Block(p)].ctrl [(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.ctrlExtSel.mask;
        l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_WR] = &d000Write[(p - pageNr(0xd000)) * pageSize];
        l1Blocks[page2L1Block(p)].ctrl [(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_WR] = bus.ctrlExtSel.mask;
    }

    // Map register reads for the page containing 0xd1ff so we can handle reads to newport/0xd1ff 
    // implementing PBI interrupt scheme 
    l1Blocks[page2L1Block(pageNr(0xd1ff))].pages[(pageNr(0xd1ff) & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD] = &d000Read[(pageNr(0xd1ff) - pageNr(0xd000)) * pageSize];
    l1Blocks[page2L1Block(pageNr(0xd1ff))].ctrl[(pageNr(0xd1ff) & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.ctrlExtSel.mask;
    
    // $D600-$D7FF, reserved for PBI devices and undecoded on a stock machine,
    // reads back from its own write shadow: 512 bytes of PBI RAM, as on the
    // 1400XL. It starts as $FF, what the XL's data bus pull-ups give an
    // unanswered read. (spiffs/stress.c's PBI bank test runs a stub at
    // $D7FD.) A VBXE at $D640 or $D740 would conflict.
    memset((void *)&d000Write[0x600], 0xff, 0x200);
    for(int p = pageNr(0xd600); p <= pageNr(0xd7ff); p++) {
        l1Blocks[page2L1Block(p)].pages[(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD] = &d000Write[(p - pageNr(0xd000)) * pageSize];
        l1Blocks[page2L1Block(p)].ctrl [(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.ctrlExtSel.mask;
    }

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
    // mpd_ only means something for $D800-$DFFF: it turns the OS math-pack
    // ROM off there so the PBI device's extSel_ response is used (extSel_
    // can't override ROM). Assert it only on those pages, reads and
    // writes, all of them extSel_ pages (mapped RWIsolated above). Rule:
    // a ctrl entry never has mpd_ without extSel_.
    for(int p = pageNr(0xd800); p <= pageNr(0xdfff); p++) {
        for(int vid : PAGESEL_EXTRA_VARIATIONS) {
            osRomEnabledPbiL1Block.ctrl[(p & pageInL1BlockMask) | PAGESEL_RD | vid] |= bus.ctrlMpd.mask;
            osRomEnabledPbiL1Block.ctrl[(p & pageInL1BlockMask) | PAGESEL_WR | vid] |= bus.ctrlMpd.mask;
        }
    }
    for(int a = 0; a < ARRAYSZ(osRomEnabledPbiL1Block.ctrl); a++) {
        const uint16_t c = osRomEnabledPbiL1Block.ctrl[a];
        if ((c & bus.ctrlMpd.mask) && !(c & bus.ctrlExtSel.mask))
            printf("MMU ERROR: PBI block ctrl[%d] = 0x%04x has mpd_ without extSel_\n", a, c);
    }

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

    mmuBuildBankBlocks(mmuState);

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
        uint8_t *mem = mmuBlock(ms, page2L1Block(p))->pages[(p & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD];
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
    for(int i = 0; i < ARRAYSZ(extMem.premap); i++) {
        L1Block *bank = ms.extBanks[i << 2];   // extBanks[] is by PORTB byte
        printf("extbank %02d: ", i); 
        if (bank == &l1Blocks[page2L1Block(pageNr(0x4000))]) {
            printf("(basemem 0x4000)\n");
            continue;
        }  
        bool found = false;
        for(int b = 0; b < ARRAYSZ(extMem.premap); b++) { 
            if (extMem.premap[b] >= 0) { 
                if (bank->pages[0 | PAGESEL_CPU | PAGESEL_RD] == extMem.banks[extMem.premap[b]]) { 
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
        if (bank == mmuBlock(ms, l1Block40)) printf ("(active)");
        printf("\n");
    }
#endif
}
