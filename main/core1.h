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
#include "const.h"
#include "mmu.h"
#include "bmon.h"
#include "profile.h"

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

