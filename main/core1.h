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

#ifdef PROFA
#undef PROFILE1
#define PROFILE1(ticks) profilers[1].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROFB
#undef PROFILE2
#undef PROFILE3
#define PROFILE2(ticks) profilers[1].add(ticks)
#define PROFILE3(ticks) profilers[2].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROFC
#undef PROFILE4
#undef PROFILE5
#define PROFILE4(ticks) profilers[1].add(ticks)
#define PROFILE5(ticks) profilers[2].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROFD
#undef PROFILE0
#define PROFILE0(ticks) profilers[1].add(ticks)
#define FAKE_CLOCK
#endif

#if 0 //  FAKE_CLOCK
#define PROFILE_BMON(ticks) {}
#define PROFILE_MMU(ticks) {}
#else
#define PROFILE_BMON(ticks) profilers[4].add(ticks)
#define PROFILE_MMU(ticks) profilers[0].add(ticks)
#endif

#ifndef TEST_SEC
#define TEST_SEC -1
#endif

//XXOPTS    
#define BUS_DETACH 
//#define FAKE_CLOCK


//#define RAM_TEST
#ifdef RAM_TEST
#undef BUS_DETACH
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

#if 0 
struct Pin {
    int gpionum;
    int bitlen;
    inline const int regRd() { return gpionum > 31 ? GPIO_IN1_REG : GPIO_IN_REG; }
    inline const int regWr() { return gpionum > 31 ? GPIO_IN1_REG : GPIO_IN_REG; }
    Pin(int n, int len = 1) : gpionum(n), bitlen(len) {}
    uint32_t const mask() { return ((1 << bitlen) - 1) << shift(); }
    int const shift() { return gpionum & 31; }
};
#endif

//GPIO0 pins
static const DRAM_ATTR int      casInh_pin = 17;
static const DRAM_ATTR int      casInh_Shift = casInh_pin;
static const DRAM_ATTR int      casInh_Mask = (0x1 << casInh_pin);               // pin 0 
static const DRAM_ATTR int      clockPin = 0;
//static const DRAM_ATTR int      clockMask = (0x1 << clockPin);
static const DRAM_ATTR int      addr0Pin = 1;
static const DRAM_ATTR int      addrShift = addr0Pin;                   // bus address - pins 1-16
static const DRAM_ATTR int      addrMask = 0xffff << addrShift;  // 
static const DRAM_ATTR int      refreshPin = 21;
static const DRAM_ATTR int      refreshMask = (1 << refreshPin);
static const DRAM_ATTR int      readWritePin = 18;
static const DRAM_ATTR int      readWriteShift = readWritePin;
static const DRAM_ATTR int      readWriteMask = (1 << readWritePin); 

//GPIO1 pins
static const DRAM_ATTR int      interruptPin = 48;
static const DRAM_ATTR int      interruptShift = (interruptPin - 32);
static const DRAM_ATTR int      interruptMask = 1 << interruptShift; 

#ifdef HAVE_RESET_PIN
static const DRAM_ATTR int      resetPin = 46;
static const DRAM_ATTR int      resetMask = 1 << (resetPin - 32); 
#endif
static const DRAM_ATTR int      mpdPin = 46;  // active low
static const DRAM_ATTR int      mpdShift = (mpdPin - 32);
static const DRAM_ATTR int      mpdMask = 1 << mpdShift; 
static const DRAM_ATTR int      extSel_Pin = 47; // active high 
static const DRAM_ATTR int      extSel_PortPin = extSel_Pin - 32 /* port1 pin*/;
static const DRAM_ATTR int      extSel_Mask = (1 << extSel_PortPin);
static const DRAM_ATTR int      data0Pin = 38;
static const DRAM_ATTR int      data0Mask = (data0Pin - 32);
static const DRAM_ATTR int      data0PortPin = data0Pin - 32;
static const DRAM_ATTR int      dataShift = data0PortPin;
static const DRAM_ATTR int      dataMask = (0xff << dataShift);

static const DRAM_ATTR int      haltPin = 35;  // pbi READY signal, low indicates halt 
static const DRAM_ATTR int      haltShift = (haltPin - 32);
static const DRAM_ATTR int      haltMask = 1 << haltShift; 

#ifdef HAVE_RESET_PIN
static const DRAM_ATTR uint32_t copyResetMask = 0x40000000;
#endif
static const DRAM_ATTR uint32_t copyMpdMask = 0x40000000;
static const DRAM_ATTR uint32_t copyDataShift = 22;
static const DRAM_ATTR uint32_t copyDataMask = 0xff << copyDataShift;

// TODO: try pin 19,20 (USB d- d+ pins). Move reset to 0 so ESP32 boot doesnt get messed up by low signal   
using std::vector;
static const vector<int> pins = {
   35,
//
// +--Clock                                  +-- CasInhAL                      +--MPD out
// | +--- ADDR 0-15                          |  +-- Read                       |  +-- ext sel out
// | |                                       |  |  +-- Refresh                 |  |  +- Interrupt out
// | |                                       |  |  |   +---DATA 0-7            |  |  |
// V V + + + + + + + +  +  +  +  +  +  +  +  V  V  V   +  +  +  +  +  +  +  +  V  V  V
   0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21, 38,39,40,41,42,43,44,45,46,47,48};
//static const int led_NO_Pin = -1;

#define bankBits 5 // TODO: change this so bankSize is defined, rest are derived.  bankBits is confusing 
#define nrBanks  (1 << bankBits)
#define bankSize  (64 * 1024 / nrBanks)
static const DRAM_ATTR uint16_t bankOffsetMask = bankSize - 1;
static const DRAM_ATTR uint16_t bankMask = ~bankOffsetMask;
static const DRAM_ATTR int bankShift = 16 - bankBits;
#define bankNr(x) ((x) >> bankShift)
static const DRAM_ATTR int BANKSEL_RD = (1 << (bankBits + 1));
static const DRAM_ATTR int BANKSEL_WR = 0;
static const DRAM_ATTR int BANKSEL_CPU = 0;

#define BUSCTL_VOLATILE volatile
#define RAM_VOLATILE //volatile

extern DRAM_ATTR RAM_VOLATILE uint8_t *banks[nrBanks * 4];
extern DRAM_ATTR uint32_t bankEnable[nrBanks * 4];
static const DRAM_ATTR int baseRamSz = 32 * 1024; // TMP: needs to be at least 32K to cover the bank window
extern DRAM_ATTR RAM_VOLATILE uint8_t atariRam[baseRamSz];
extern DRAM_ATTR RAM_VOLATILE uint8_t cartROM[];
extern DRAM_ATTR RAM_VOLATILE uint8_t pbiROM[2 * 1024];
extern DRAM_ATTR RAM_VOLATILE uint8_t D000Write[0x800];
extern DRAM_ATTR RAM_VOLATILE uint8_t D000Read[0x800];

extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinDisableMask; // = dataMask | extSel_Mask;
extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinEnableMask;  // = 0;
extern BUSCTL_VOLATILE DRAM_ATTR uint32_t pinInhibitMask;
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

static const DRAM_ATTR int numProfilers = 5;
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
static const DRAM_ATTR uint16_t bankNr_d300 = bankNr(0xd300);
static const DRAM_ATTR uint16_t bankNr_d100 = bankNr(0xd100);
static const DRAM_ATTR uint16_t bankNr_d500 = bankNr(0xd500);

