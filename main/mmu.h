#pragma once
//#pragma GCC optimize("O1")

#include "esp_attr.h"
#include <inttypes.h>
#include "pinDefs.h"

#ifndef DRAM_ATTR
#error only to pacify up vscode, DRAM_ATTR is required  
#define DRAM_ATTR
#endif

#define bankL1Bits 2
#define pageBits 8
#define nrPages (1 << pageBits)
#define pageSize  (64 * 1024 / nrPages)
static constexpr DRAM_ATTR uint16_t pageOffsetMask = pageSize - 1;
static constexpr DRAM_ATTR uint16_t pageMask = ~pageOffsetMask;
static constexpr DRAM_ATTR int pageShift = 16 - pageBits;
#define pageNr(x) ((x) >> pageShift)
static constexpr DRAM_ATTR int PAGESEL_RD = (1 << (pageBits - bankL1Bits));
static constexpr DRAM_ATTR int PAGESEL_WR = 0;
static constexpr DRAM_ATTR int PAGESEL_VID = (1 << (pageBits - bankL1Bits + 1));
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

#define baseMemSz (2 * 1024) 

//extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinDriveMask;  // = 0;
//extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinEnableMask;
//extern DRAM_ATTR int busWriteDisable;     // = 0;
DRAM_ATTR constexpr uint32_t pinReleaseMask = bus.irq_.mask | bus.data.mask | bus.extSel.mask | bus.mpd.mask | bus.halt_.mask;

struct BankL1Entry;

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
uint8_t *mmuCheckRangeMapped(uint16_t addr, uint16_t len);
void mmuMapBankRO(uint16_t addr, BankL1Entry *b); 
void mmuRemapBankBaseRam(uint16_t addr);
void mmuDebugPrint();


//extern RAM_VOLATILE uint8_t *pages[nrPages * (1 << PAGESEL_EXTRA_BITS)];
//extern uint32_t pageEnable[nrPages * (1 << PAGESEL_EXTRA_BITS)];
extern RAM_VOLATILE uint8_t *baseMemPages[nrPages];
extern RAM_VOLATILE uint8_t dummyRam[pageSize];
extern RAM_VOLATILE uint8_t d000Write[0x800];
extern RAM_VOLATILE uint8_t d000Read[0x800];
extern RAM_VOLATILE uint8_t *screenMem;
extern RAM_VOLATILE uint8_t atariRam[baseMemSz];
extern RAM_VOLATILE uint8_t cartROM[];
extern RAM_VOLATILE uint8_t pbiROM[2 * 1024];

static constexpr DRAM_ATTR uint16_t pageNr_d301 = pageNr(0xd301);
static constexpr DRAM_ATTR uint16_t pageNr_d1ff = pageNr(0xd1ff);
static constexpr DRAM_ATTR uint16_t pageNr_d500 = pageNr(0xd500);

// sketched in enough placeholder for a new first-level of inderection in the page tables 
// "BankL1", probably 8k pages, allowing rapid swapping in/out of cartridge banks. 
// Sketched in just enough to simulate timing in the core1 loop 
#define bankL1Bits 2
#define nrL1Banks (1 << bankL1Bits)
static constexpr DRAM_ATTR uint16_t bankL1Size = (64 * 1024 / nrL1Banks);
#define bankL1Shift (16 - bankL1Bits)
#define bankL1Nr(x) ((x) >> bankL1Shift)
static constexpr DRAM_ATTR uint16_t bankL1OffsetMask = (bankL1Size - 1);
#define pagesPerBank (nrPages / nrL1Banks)
#define pageInBankMask (pagesPerBank - 1)
#define page2bank(p) ((p) >> (bankL1Shift - pageShift))

struct BankL1Entry { 
    uint8_t *pages[pagesPerBank * (1 << PAGESEL_EXTRA_BITS)]; // array a page data pointers
    uint32_t ctrl[pagesPerBank * (1 << PAGESEL_EXTRA_BITS)];  // array of page bus control bits 
};
//extern RAM_VOLATILE BankL1Entry dummyBankRosRomDisabledBank, osRomEnabledBank;
extern RAM_VOLATILE BankL1Entry banksL1[nrL1Banks];
//extern RAM_VOLATILE BankL1Entry *banks[nrL1Banks];

extern uint8_t lastPageOffset[nrPages * (1 << PAGESEL_EXTRA_BITS)];

struct MmuState {
    BankL1Entry *banks[nrL1Banks];
    BankL1Entry *basicEnBankMux[2];
    BankL1Entry *osEnBankMux[4];
    BankL1Entry *cartBanks[32];
    BankL1Entry *extBanks[32];
};

// static constexpr DRAM_ATTR int mmuStateSize = sizeof(MmuState);
extern RAM_VOLATILE MmuState mmuState;
extern RAM_VOLATILE MmuState mmuStateSaved;
extern RAM_VOLATILE MmuState mmuStateDisabled;

static constexpr DRAM_ATTR int pageD5 = pageNr(0xd500) | PAGESEL_CPU | PAGESEL_WR;   // page to watch for cartidge control accesses
static constexpr DRAM_ATTR int bank40 = page2bank(pageNr(0x4000)); // bank to remap for ext mem banking  
static constexpr DRAM_ATTR int bank80 = page2bank(pageNr(0x8000)); // bank to remap for cart control 
static constexpr DRAM_ATTR int bankA0 = page2bank(pageNr(0xa000)); // bank to remap for ext mem banking  
static constexpr DRAM_ATTR int bankC0 = page2bank(pageNr(0xc000)); // bank to remap for os rom enable bit 
