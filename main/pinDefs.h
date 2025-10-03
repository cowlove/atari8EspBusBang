#pragma once 
#include <vector>
#include "esp_attr.h"
#include "soc/gpio_reg.h"

using std::vector;

template<int n, int len=1> struct Pin {
   static const int pin = n;
   static const int bits = len;
   static const int shift = pin & 31;
   static const int mask = ((1 << bits) - 1) << shift;
   static const int regRd = pin > 31 ? GPIO_IN1_REG : GPIO_IN_REG; 
   static const int regWr = pin > 31 ? GPIO_OUT1_REG : GPIO_OUT_REG; 
};

using std::vector;
static const vector<int> gpios = {
// GPIO0 PINS:
//
// +--Audio/Unused                           +-- Read/Write
// | +--- ADDR 0-15                          |  +-- Halt_ input
// | |                                       |  |  +-- Refresh
// | |                                       |  |  | 
   0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,

// GPIO1 PINS:                          
//    
// +--HALT_ output                                     
// |   +-Clock                          +--MPD out  
// |   |   +-IRQ_ output                |  +-- ext sel out   
// |   |   |    +--DATA 0-7             |  |  
// |   |   |    |                       |  |    
// V   V   V    V  +  +  +  +  +  +  +  V  V    
   35, 36, 37, 38,39,40,41,42,43,44,45,46,47/*,48*/
};

static const DRAM_ATTR struct BusPins {
   // GPIO0 pins, primarily input 
   static const Pin<36>     clock; 
   static const Pin<1,16>  addr;
   static const Pin<18>    extDecode;
   static const Pin<17>    rw;
   static const Pin<21>    refresh_; // active low 

   // GPIO1 pins, primarily output 
   static const Pin<35>    halt_;    // active low 
   static const Pin<38, 8> data;
   static const Pin<46>    mpd;
   static const Pin<47>    extSel;
   static const Pin<37>    irq_;     // active low 
} bus;

DRAM_ATTR static const int ledPin = 48;
