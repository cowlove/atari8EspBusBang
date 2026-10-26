#pragma once
//#pragma GCC optimize("O1")

#include "esp_attr.h"
#include <inttypes.h>
#include "pinDefs.h"
#include "util.h"

#ifndef DRAM_ATTR
#error only to pacify up vscode, DRAM_ATTR is required  
#define DRAM_ATTR
#endif

#define l1BlockBits 2
#define pageBits 8
#define nrPages (1 << pageBits)
#define pageSize  (64 * 1024 / nrPages)
static constexpr DRAM_ATTR uint16_t pageOffsetMask = pageSize - 1;
static constexpr DRAM_ATTR uint16_t pageMask = ~pageOffsetMask;
static constexpr DRAM_ATTR int pageShift = 16 - pageBits;
#define pageNr(x) ((x) >> pageShift)
static constexpr DRAM_ATTR int PAGESEL_RD = (1 << (pageBits - l1BlockBits));
static constexpr DRAM_ATTR int PAGESEL_WR = 0;
static constexpr DRAM_ATTR int PAGESEL_VID = (1 << (pageBits - l1BlockBits + 1));
static constexpr DRAM_ATTR int PAGESEL_CPU = 0;

//#define USE_PAGESEL_VID
#ifdef USE_PAGESEL_VID
static constexpr DRAM_ATTR int PAGESEL_EXTRA_BITS = 2;
static constexpr DRAM_ATTR uint32_t PAGESEL_VARIANT_SET[] = {PAGESEL_CPU, PAGESEL_VID};
#else
static constexpr DRAM_ATTR int PAGESEL_EXTRA_BITS = 1;
static constexpr DRAM_ATTR uint32_t PAGESEL_EXTRA_VARIATIONS[] = {PAGESEL_CPU};
#endif

#define BUSCTL_VOLATILE volatile
#define RAM_VOLATILE //volatile

extern int baseMemSz;

//extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinDriveMask;  // = 0;
//extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinEnableMask;
//extern DRAM_ATTR int busWriteDisable;     // = 0;
extern BUSCTL_VOLATILE DRAM_ATTR /*constexpr*/ uint32_t pinReleaseMask;// = bus.irq_.mask | bus.data.mask | bus.extSel.mask | bus.mpd.mask | bus.halt_.mask;

struct L1Block;
struct MmuState;

void mmuInit(); 
void mmuOnChange(bool force = false);
void mmuMapPbiRom(bool pbiEn, bool osEn);
void mmuRemapBaseRam(uint16_t start, uint16_t end);
void mmuUnmapRange(uint16_t start, uint16_t end);
void mmuMapRangeRO(uint16_t start, uint16_t end, uint8_t *mem); 
void mmuMapRangeRWIsolated(uint16_t start, uint16_t end, uint8_t *mem); 
void mmuMapRangeRW(uint16_t start, uint16_t end, uint8_t *mem);
uint8_t *mmuAllocAddBaseRam(uint16_t start, uint16_t end);
void mmuAddBaseRam(uint16_t start, uint16_t end, uint8_t *mem);
uint8_t *mmuCheckRangeMapped(MmuState &ms, uint16_t addr, uint16_t len);
void mmuMapL1BlockRO(uint16_t addr, L1Block *block);
void mmuRemapL1BlockBaseRam(uint16_t addr);
void mmuDebugPrint();


//extern RAM_VOLATILE uint8_t *pages[nrPages * (1 << PAGESEL_EXTRA_BITS)];
//extern uint32_t pageEnable[nrPages * (1 << PAGESEL_EXTRA_BITS)];
extern RAM_VOLATILE uint8_t *baseMemPages[nrPages];
extern RAM_VOLATILE uint8_t dummyRam[pageSize];
extern RAM_VOLATILE uint8_t d000Write[0x800];
extern RAM_VOLATILE uint8_t d000Read[0x800];
extern RAM_VOLATILE uint8_t *screenMem;
extern RAM_VOLATILE uint8_t *atariRam;
extern RAM_VOLATILE uint8_t cartROM[];
#ifdef ATARI800_SIM
extern "C" RAM_VOLATILE uint8_t pbiROM[2 * 1024];
#else
extern RAM_VOLATILE uint8_t pbiROM[2 * 1024];
#endif

static constexpr DRAM_ATTR uint16_t pageNr_d301 = pageNr(0xd301);
static constexpr DRAM_ATTR uint16_t pageNr_d1ff = pageNr(0xd1ff);
static constexpr DRAM_ATTR uint16_t pageNr_d500 = pageNr(0xd500);

// First-level MMU blocks cover 16 KiB of the Atari address space and contain page mappings.
// Sketched in just enough to simulate timing in the core1 loop 
#define l1BlockBits 2
#define nrL1Blocks (1 << l1BlockBits)
static constexpr DRAM_ATTR uint16_t l1BlockSize = (64 * 1024 / nrL1Blocks);
#define l1BlockShift (16 - l1BlockBits)
#define l1BlockNr(x) ((x) >> l1BlockShift)
static constexpr DRAM_ATTR uint16_t l1BlockOffsetMask = (l1BlockSize - 1);
#define pagesPerL1Block (nrPages / nrL1Blocks)
#define pageInL1BlockMask (pagesPerL1Block - 1)
#define page2L1Block(p) ((p) >> (l1BlockShift - pageShift))

// TODO: rename the the 16K L1Block mappings to "blocks" or something to avoid widespread confusion with
struct L1Block {
    uint8_t *pages[pagesPerL1Block * (1 << PAGESEL_EXTRA_BITS)]; // array a page data pointers
    uint16_t ctrl[pagesPerL1Block * (1 << PAGESEL_EXTRA_BITS)];  // array of page bus control bits
};

extern RAM_VOLATILE L1Block l1Blocks[nrL1Blocks];

extern uint8_t lastPageOffset[nrPages * (1 << PAGESEL_EXTRA_BITS)];

struct MmuState {
    L1Block *l1Blocks[nrL1Blocks];
    L1Block *basicEnL1BlockMux[2];
    L1Block *osEnL1BlockMux[4];
    L1Block *cartBanks[32];
    L1Block *extBanks[32];
    inline IRAM_ATTR void safeCopy(MmuState &b) { 
        L1Block **dest = (L1Block **)this;
        L1Block **end = (L1Block **)(this + 1);
        L1Block **src = (L1Block **)&b;
        while(dest != end) 
            *(dest++) = *(src++);

    }
};
void mmuDebugPrintMmuState(MmuState &);

extern RAM_VOLATILE MmuState mmuState;
extern RAM_VOLATILE MmuState mmuStateSaved;
extern RAM_VOLATILE MmuState mmuStateDisabled;
extern RAM_VOLATILE L1Block dummyL1Block;

static constexpr DRAM_ATTR int pageD5 = pageNr(0xd500) | PAGESEL_CPU | PAGESEL_WR;   // page to watch for cartidge control accesses
static constexpr DRAM_ATTR int l1Block40 = page2L1Block(pageNr(0x4000)); // L1 block for extended memory
static constexpr DRAM_ATTR int l1Block80 = page2L1Block(pageNr(0x8000)); // L1 block for cartridge mapping
static constexpr DRAM_ATTR int l1BlockA0 = page2L1Block(pageNr(0xa000)); // L1 block for extended memory
static constexpr DRAM_ATTR int l1BlockC0 = page2L1Block(pageNr(0xc000)); // L1 block for OS ROM mapping
