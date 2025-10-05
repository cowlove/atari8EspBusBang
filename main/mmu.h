#pragma once
#pragma GCC optimize("O1")

#include "esp_attr.h"
#include <inttypes.h>

#define pageBits 8
#define nrPages (1 << pageBits)
#define pageSize  (64 * 1024 / nrPages)
static const DRAM_ATTR uint16_t pageOffsetMask = pageSize - 1;
static const DRAM_ATTR uint16_t pageMask = ~pageOffsetMask;
static const DRAM_ATTR int pageShift = 16 - pageBits;
#define pageNr(x) ((x) >> pageShift)
static const DRAM_ATTR int PAGESEL_RD = (1 << (pageBits));
static const DRAM_ATTR int PAGESEL_WR = 0;
static const DRAM_ATTR int PAGESEL_VID = (1 << (pageBits + 1));
static const DRAM_ATTR int PAGESEL_CPU = 0;
static const DRAM_ATTR int PAGESEL_EXTRA_BITS = 2;


#define BUSCTL_VOLATILE volatile
#define RAM_VOLATILE //volatile

#ifdef BOOT_SDX
#define baseMemSz (64 * 1024) 
#else
#define baseMemSz 0xc000 // (48 * 1024) 
#endif

extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinReleaseMask; // = dataMask | extSel_Mask;
extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinDriveMask;  // = 0;
extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinEnableMask;
extern DRAM_ATTR int busWriteDisable;     // = 0;

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


extern RAM_VOLATILE uint8_t *pages[nrPages * (1 << PAGESEL_EXTRA_BITS)];
extern uint32_t pageEnable[nrPages * (1 << PAGESEL_EXTRA_BITS)];
extern RAM_VOLATILE uint8_t *baseMemPages[nrPages];
extern RAM_VOLATILE uint8_t dummyRam[pageSize];
extern RAM_VOLATILE uint8_t d000Write[0x800];
extern RAM_VOLATILE uint8_t d000Read[0x800];
extern RAM_VOLATILE uint8_t *screenMem;
extern RAM_VOLATILE uint8_t atariRam[baseMemSz];
extern RAM_VOLATILE uint8_t cartROM[];
extern RAM_VOLATILE uint8_t pbiROM[2 * 1024];

static const DRAM_ATTR uint16_t pageNr_d301 = pageNr(0xd301);
static const DRAM_ATTR uint16_t pageNr_d1ff = pageNr(0xd1ff);
static const DRAM_ATTR uint16_t pageNr_d500 = pageNr(0xd500);


// sketched in enough placeholder for a new first-level of inderection in the page tables 
// "BankL1", probably 8k pages, allowing rapid swapping in/out of cartridge banks. 
// Sketched in just enough to simulate timing in the core1 loop 
#define bankL1Bits 4
#define nrL1Banks (1 << bankL1Bits)
static const DRAM_ATTR uint16_t bankL1Size = (64 * 1024 / nrL1Banks);
#define bankL1Shift (16 - bankL1Bits)
#define bankL1Nr(x) ((x) >> bankL1Shift)
static const DRAM_ATTR uint16_t bankL1OffsetMask = (bankL1Size - 1);
#define pagesPerBank (nrPages / nrL1Banks)
#define pageInBankMask (pagesPerBank - 1)

struct BankL1Entry { 
    uint8_t *pages[pagesPerBank]; // array a page data pointers
    uint32_t ctrl[pagesPerBank];  // array of page bus control bits 
};

extern RAM_VOLATILE BankL1Entry banksL1[nrL1Banks * (1 << PAGESEL_EXTRA_BITS)];

