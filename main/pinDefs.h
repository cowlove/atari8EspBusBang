#pragma once 
#include "esp_attr.h"
#include <vector>
using std::vector;

// TODO: try pin 19,20 (USB d- d+ pins). Move reset to 0 so ESP32 boot doesnt get messed up by low signal   
using std::vector;
static const vector<int> pins = {
// GPIO0 PINS:
//
// +--Clock                                  +-- CasInhAL
// | +--- ADDR 0-15                          |  +-- Read
// | |                                       |  |  +-- Refresh
// | |                                       |  |  | 
   0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,

// GPIO1 PINS:                         +--MPD out
//                                     |  +-- ext sel out   
//                                     |  |  +- Interrupt out
// +--HALT     +---DATA 0-7            |  |  |   
// |           |                       |  |  | 
// V           V  +  +  +  +  +  +  +  V  V  V   
   35, 36, 37, 38,39,40,41,42,43,44,45,46,47,48
};

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

