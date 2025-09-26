#pragma once 
#pragma GCC optimize("O1")
#include <vector>
using std::vector;
#ifndef CSIM
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_sig_map.h"
#include "hal/gpio_ll.h"
#include "rom/gpio.h"
#include <esp_timer.h>
#include "driver/gpio.h"

#include "gitVersion.h"

#if 0
// TMP: investigate removing these, should be unneccessary due to linker script
#undef DRAM_ATTR
#define DRAM_ATTR
#undef IRAM_ATTR
#define IRAM_ATTR 
#endif // #if 0 

#define IFLASH_ATTR __attribute__((noinline))

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wattributes"

#define ASM(x) __asm__ __volatile__ (x)
#else // CSIM
#define ASM(x) 0 
#endif // CSIM

void IRAM_ATTR iloop_pbi();

// Usable pins
// 0-18 21            (20)
// 38-48 (6-16)     (11)
//pin 01234567890123456789012345678901
//    11111111111111111110001000000000
//    00000011111111110000000000000000

//data 8
//addr 16
//clock sync halt write 
// TODO: investigate GPIO input filter, GPIO output sync 

//GPIO0 bits: TODO rearrange bits so addr is in low bits and avoids needed a shift
// Need 19 pines on gpio0: ADDR(16), clock, casInh, RW

#define PROFILE0(a) {}
#define PROFILE1(a) {}
#define PROFILE2(a) {}
#define PROFILE3(a) {}
#define PROFILE4(a) {}
#define PROFILE5(a) {}

#ifdef PROF0
#undef PROFILE0
#define PROFILE0(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROF1
#undef PROFILE1
#define PROFILE1(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROF2
#undef PROFILE2
#define PROFILE2(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROF3
#undef PROFILE3
#define PROFILE3(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROF4
#undef PROFILE4
#define PROFILE4(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif

#ifdef FAKE_CLOCK
#define PROFILE_BMON(ticks) {}
#define PROFILE_MMU(ticks) {}
#else
#define PROFILE_BMON(ticks) profilers[1].add(ticks)
#define PROFILE_MMU(ticks) profilers[0].add(ticks)
#endif

#ifndef TEST_SEC
#define TEST_SEC -1
#endif

static const DRAM_ATTR struct {
#ifdef FAKE_CLOCK
   bool fakeClock     = 1; 
   int histRunSec   = TEST_SEC;
#else 
   bool fakeClock     = 0;
   int histRunSec   = 2 * 3600;
#endif 
   bool testPins      = 0;
   bool watchPins     = 0;      // loop forever printing pin values w/ INPUT_PULLUP
   bool dumpSram      = 0;   ;
   bool timingTest    = 0;
   bool bitResponse   = 0;
   bool core0Led      = 0; // broken, PBI loop overwrites entire OUT1 register including 
   int dumpPsram      = INT_MAX;
   bool forceMemTest  = 0;
   bool tcpSendPsram  = 0;
   bool histogram     = 1;
} opt;

#define pageBits 5
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

#define baseMemSz (40 * 1024) 
extern DRAM_ATTR RAM_VOLATILE uint8_t *pages[nrPages * 4];
extern DRAM_ATTR uint32_t pageEnable[nrPages * 4];
extern DRAM_ATTR RAM_VOLATILE uint8_t atariRam[baseMemSz];
extern DRAM_ATTR RAM_VOLATILE uint8_t cartROM[];
extern DRAM_ATTR RAM_VOLATILE uint8_t pbiROM[2 * 1024];
extern DRAM_ATTR RAM_VOLATILE uint8_t d000Write[0x800];
extern DRAM_ATTR RAM_VOLATILE uint8_t d000Read[0x800];
extern DRAM_ATTR RAM_VOLATILE uint8_t *baseMemPages[nrPages];

extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinReleaseMask; // = dataMask | extSel_Mask;
extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinDriveMask;  // = 0;
extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinEnableMask;
extern DRAM_ATTR int busWriteDisable;     // = 0;

struct Hist2 { 
    static const DRAM_ATTR int maxBucket = 512; // must be power of 2
    int buckets[maxBucket];
    inline void clear() { for(int i = 0; i < maxBucket; i++) buckets[i] = 0; }
    inline void add(uint32_t x) { buckets[x & (maxBucket - 1)]++; }
    Hist2() { clear(); }
    int64_t count() {
        int64_t sum = 0; 
        for(int i = 0; i < maxBucket; i++) sum += buckets[i];
        return sum;
    }
};

static const DRAM_ATTR int numProfilers = 2;
extern DRAM_ATTR Hist2 profilers[numProfilers];

#if 0
#undef REG_READ
#undef REG_WRITE
#if 1
#define REG_READ(r) (*((volatile uint32_t *)r))
#define REG_WRITE(r,v) do { *((volatile uint32_t *)r) = (v); } while(0)
#else
#define REG_READ(r) (*((uint32_t *)r))
#define REG_WRITE(r,v) do { *((uint32_t *)r) = (v); } while(0)
#endif 
#endif 

static const  DRAM_ATTR int PDIMSK = 0x249;

//static const int pbiDeviceNum = ; // also change in pbirom.asm
static const DRAM_ATTR int pbiDeviceNumMask = 0x2;
static const DRAM_ATTR int pbiDeviceNumShift = 1;

static const DRAM_ATTR int bmonR0Shift = 8;
static const DRAM_ATTR unsigned int bmonArraySz = 1024;  // must be power of 2
static const DRAM_ATTR unsigned int bmonArraySzMask = bmonArraySz - 1;
extern DRAM_ATTR uint32_t bmonArray[bmonArraySz];
extern volatile DRAM_ATTR unsigned int bmonHead;
extern volatile DRAM_ATTR unsigned int bmonTail;

const static DRAM_ATTR uint16_t _0x1ff = 0x1ff;
const static DRAM_ATTR uint16_t _0x301 = 0x301;
const static DRAM_ATTR uint16_t _0x4000 = 0x4000;
const static DRAM_ATTR uint16_t _0x7fff = 0x7fff;
const static DRAM_ATTR uint16_t _0xffff = 0xffff;
const static DRAM_ATTR uint16_t _0xc000 = 0xc000;
const static DRAM_ATTR uint16_t _0xcfff = 0xcfff;
const static DRAM_ATTR uint16_t _0x5000 = 0x5000;
const static DRAM_ATTR uint16_t _0x57ff = 0x57ff;
const static DRAM_ATTR uint16_t _0xe000 = 0xe000;
const static DRAM_ATTR uint16_t _0xa000 = 0xa000;
const static DRAM_ATTR uint16_t _0xbfff = 0xbfff;
const static DRAM_ATTR uint16_t _0xd800 = 0xd800;
const static DRAM_ATTR uint16_t _0xdfff = 0xdfff;
const static DRAM_ATTR uint16_t _0x8000 = 0x8000;
const static DRAM_ATTR uint16_t _0x9fff = 0x9fff;
static const DRAM_ATTR uint32_t _0xffffffff = ~0;

static const DRAM_ATTR uint16_t _0xd830 = 0xd830;
static const DRAM_ATTR uint16_t _0xd840 = 0xd840;
static const DRAM_ATTR uint16_t _0xd301 = 0xd301;
static const DRAM_ATTR uint16_t _0xd1ff = 0xd1ff;
static const DRAM_ATTR uint16_t _0xd500 = 0xd500;
static const DRAM_ATTR uint16_t _0xd300 = 0xd300;
static const DRAM_ATTR uint16_t _0xff00 = 0xff00;
static const DRAM_ATTR uint16_t pageNr_d301 = pageNr(0xd301);
static const DRAM_ATTR uint16_t pageNr_d1ff = pageNr(0xd1ff);
static const DRAM_ATTR uint16_t pageNr_d500 = pageNr(0xd500);

