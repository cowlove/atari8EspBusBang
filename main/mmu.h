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


extern RAM_VOLATILE uint8_t *pages[nrPages * 4];
extern uint32_t pageEnable[nrPages * 4];
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
