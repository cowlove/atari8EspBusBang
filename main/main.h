#pragma once 
//#pragma GCC optimize("O1")
#include <string>
#include <functional>
#ifndef CSIM
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_sig_map.h"
#include "hal/gpio_ll.h"
#include "rom/gpio.h"
#include <esp_timer.h>
#include "driver/gpio.h"

#include "gitVersion.h"
#include "const.h"
#include "mmu.h"
#include "bmon.h"
#include "profile.h"
#include "led.h"

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



static const DRAM_ATTR struct {
#ifdef FAKE_CLOCK
   bool fakeClock     = 1; 
#else 
   bool fakeClock     = 0;
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

extern DRAM_ATTR std::string exitReason;
extern DRAM_ATTR int exitFlag;
extern DRAM_ATTR int elapsedSec;
extern DRAM_ATTR int lastScreenShot;
extern DRAM_ATTR int ioCount, pbiInterruptCount, memWriteErrors, unmapCount, lastIoSec,
    watchDogCount, spuriousHaltCount, haltCount, httpRequests, sysMonitorRequested;
extern DRAM_ATTR uint32_t lastVblankTsc;

// Macro gibberish to define EVERYN_TICKS(n) { /*code executed once every n ticks */ }
#define TOKEN_PASTE(A, B) A##B
#define CONCAT_HELPER(A, B) TOKEN_PASTE(A,B)
#define UNIQUE_LOCAL(A) CONCAT_HELPER(A, __LINE__)
#define EVERYN_TICKS(ticks) \
    static DRAM_ATTR uint32_t UNIQUE_LOCAL(lastTsc) = XTHAL_GET_CCOUNT(); \
    static constexpr DRAM_ATTR uint32_t UNIQUE_LOCAL(interval) = (ticks); \
    const uint32_t UNIQUE_LOCAL(tsc) = XTHAL_GET_CCOUNT(); \
    bool UNIQUE_LOCAL(doLoop) = false; \
    if(UNIQUE_LOCAL(tsc) - UNIQUE_LOCAL(lastTsc) > \
        UNIQUE_LOCAL(interval)) {\
        UNIQUE_LOCAL(lastTsc) += UNIQUE_LOCAL(interval); \
        UNIQUE_LOCAL(doLoop) = true; \
    } \
    if (UNIQUE_LOCAL(doLoop))

#define EVERYN_TICKS_NO_CATCHUP(ticks) \
    static DRAM_ATTR uint32_t UNIQUE_LOCAL(lastTsc) = XTHAL_GET_CCOUNT(); \
    static constexpr DRAM_ATTR uint32_t UNIQUE_LOCAL(interval) = (ticks); \
    const uint32_t UNIQUE_LOCAL(tsc) = XTHAL_GET_CCOUNT(); \
    bool UNIQUE_LOCAL(doLoop) = false; \
    if(UNIQUE_LOCAL(tsc) - UNIQUE_LOCAL(lastTsc) > \
        UNIQUE_LOCAL(interval)) {\
        UNIQUE_LOCAL(lastTsc) = UNIQUE_LOCAL(tsc); \
        UNIQUE_LOCAL(doLoop) = true; \
    } \
    if (UNIQUE_LOCAL(doLoop))


struct __attribute__((packed)) AtariDCB { 
   uint8_t 
    DDEVIC,
    DUNIT,
    DCOMND,
    DSTATS,
    DBUFLO,
    DBUFHI,
    DTIMLO,
    DUNUSED,
    DBYTLO,
    DBYTHI,
    DAUX1,
    DAUX2;
};

struct __attribute__((packed)) AtariIOCB { 
    uint8_t ICHID,  // handler 
            ICDNO,  // Device number
            ICCOM,  // Command byte 
            ICSTA,  // Status returned
            ICBAL,  // Buffer address (points to 0x9b-terminated string for open command)
            ICBAH,
            ICPTL,  // Address of driver put routine
            ICPTH,
            ICBLL,  // Buffer length 
            ICBLH,
            ICAX1,
            ICAX2,
            ICAX3,
            ICAX4,
            ICAX5,
            ICAX6;
};

static const DRAM_ATTR struct AtariDefStruct {
    int IOCB0 = 0x340;
    int ZIOCB = 0x20;
    int NUMIOCB = 0x8;
    int IOCB_CMD_CLOSE = 0xc;
    int IOCB_CMD_OPEN = 0x3;
    int IOCB_OPEN_READ = 0x4;
    int IOCB_OPEN_WRITE = 0x8;
    int NEWPORT = 0x31ff;
    int PBI_COPYBUF = 0xdc00;
} AtariDef;

static const DRAM_ATTR struct { 
    AtariDCB *dcb = (AtariDCB *)&atariRam[0x300];
    AtariIOCB *ziocb = (AtariIOCB *)&atariRam[0x20];
    AtariIOCB *iocb0 = (AtariIOCB *)&atariRam[0x320];
} atariMem;

struct spiffs_t; 
extern struct spiffs_t *spiffs_fs;

class LineBuffer {
public:
        char line[128];
        int len = 0;
        int IRAM_ATTR add(char c, std::function<void(const char *)> f = NULL);
        void IRAM_ATTR add(const char *b, int n, std::function<void(const char *)> f);
        void IRAM_ATTR add(const uint8_t *b, int n, std::function<void(const char *)> f);
};

IRAM_ATTR void enableBus();
IRAM_ATTR void disableBus();
IRAM_ATTR void clearInterrupt();

IRAM_ATTR void putKey(char c);

#ifndef BUS_ANALYZER
#define BUS_ANALYZER 0
#endif


