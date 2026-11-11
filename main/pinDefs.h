#pragma once 
#include <vector>
#include "esp_attr.h"
#include "sdkconfig.h"
#include "soc/gpio_reg.h"

using std::vector;

template<int n, int len=1> struct Pin {
   static constexpr int pin = n;
   static constexpr int bits = len;
   static constexpr int shift = pin & 31;
   static constexpr int mask = ((1 << bits) - 1) << shift;
   static constexpr int maskInverse = ~(((1 << bits) - 1) << shift);
#ifndef CSIM   // csim's GPIO registers are variables (csim.h), not addresses
   static constexpr int regRd = pin > 31 ? GPIO_IN1_REG : GPIO_IN_REG; 
   static constexpr int regWr = pin > 31 ? GPIO_OUT1_REG : GPIO_OUT_REG; 
#endif
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

// GPIO21 is the Atari refresh_ input, except in Sidecar builds, where it is
// the link's RX (sidecarRxPin below). HAVE_REFRESH gates every use of
// bus.refresh_.
#if !CONFIG_A8BB_SIDECAR_ENABLE
#define HAVE_REFRESH 1
#endif

static const DRAM_ATTR struct BusPins {
   // GPIO0 pins, primarily input 
   static const Pin<36>     clock; 
   static const Pin<1,16>  addr;
   static const Pin<18>    extDecode;
   static const Pin<17>    rw;
#ifdef HAVE_REFRESH
   static const Pin<21>    refresh_; // active low 
#endif
   // GPIO1 pins, primarily output 
   static const Pin<35>    halt_;    // active low 
   static const Pin<38, 8> data;
   static const Pin<46>    mpd;
   static const Pin<47>    extSel;
   static const Pin<37>    irq_;     // active low 
} bus;

// GPIO48 is shared: the status LED when free; the profiling pin core 1
// drives in PROFILE_MODE=PIN (PROFILE_GPIO_PIN_OUTPUT); or the Sidecar link's
// BCLK (A8BB_SIDECAR_ENABLE). PROFILE_MODE=GPIO only samples it as an input
// (the LED stays off so nothing drives it), so it works with the Sidecar.
// LEGACY profiling doesn't use it.
#if CONFIG_A8BB_SIDECAR_ENABLE && defined(PROFILE_GPIO_PIN_OUTPUT)
#error "PROFILE_MODE=PIN (drives GPIO48) can't be used with A8BB_SIDECAR_ENABLE"
#endif
#if CONFIG_A8BB_SIDECAR_ENABLE || defined(PROFILE_GPIO_SAMPLE)
DRAM_ATTR static const int ledPin = -1;
#define HAS_STATUS_LED 0
#else
DRAM_ATTR static const int ledPin = 48;
#define HAS_STATUS_LED 1
#endif
DRAM_ATTR static const int profPin = 48;

#if CONFIG_A8BB_SIDECAR_ENABLE
// Host pins for the Sidecar link (see main/sidecarHost.h). BCLK takes GPIO48
// (above). CSIM wires virtual numbers to the Sidecar's pins instead.
//
// Hardware: provisional, matching the i2sBench wired pair (host GPIO0 ->
// Sidecar GPIO37, Sidecar GPIO35 -> host GPIO21, host GPIO48 BCLK ->
// Sidecar GPIO36; the host drives BCLK). Neither TX nor RX pin is free on
// the current host board:
// - GPIO0 (TX) is routed to the PBI bus audio input, and is a strapping pin.
//   The line's loading keeps loopback from ever locking on at least one test
//   pool board (miner6 board 5). Future revisions isolate GPIO0; any Atari
//   audio input would come from the Sidecar, which has free pins.
// - GPIO21 (RX) is the Atari refresh_ input, so Sidecar builds drop bus.refresh_
//   (no HAVE_REFRESH).
// TODO: real pins.
static const int sidecarBclkPin = 48;
#ifdef CSIM
static const int sidecarTxPin = 100, sidecarRxPin = 101;
#else
static const int sidecarTxPin = 0, sidecarRxPin = 21;
#endif
#endif

// EXTSEL_DEDIC (experiment, -D only): drive extSel_ from core 1's
// dedicated GPIO output instead of toggling its GPIO_ENABLE1 bit, so the
// pin moves within a few ticks of the instruction instead of after an APB
// store (~25 ticks on hardware). The pad stays open drain, with its output
// enable left on; the GPIO matrix inverts the channel, so writing 1 pulls
// the line low (asserted) and 0 leaves it to the pull-up.
//   0: off (production)
//   1: assert at P2; release in each path's tail, next to the APB release
//   2: assert at P2; release at the top of the cycle, right after the poll
// mpd_ (sampled at the same phi2 edge) goes the same way, on channel 2.
// The ctrl[] tables keep both bits as before: core 1 tests them in the
// pin-enable word, and the APB enable write is a no-op for those pins. It
// relies on mmu.cpp's rule that a ctrl entry never has mpd_ without
// extSel_: mpd_ is tested only once extSel_ is set. Core 1's channel 0 is
// the profile pin. With RELEASE_PLACEMENT 1-3, use 2.
#ifndef EXTSEL_DEDIC
#define EXTSEL_DEDIC 0
#endif
#if EXTSEL_DEDIC && defined(CSIM)
#error "EXTSEL_DEDIC needs core 1's dedicated GPIO; csim models extSel_ by its GPIO_ENABLE1 bit"
#endif
#if EXTSEL_DEDIC
static constexpr int extSelDedicChannel = 1;
static constexpr uint32_t extSelDedicMask = 1u << extSelDedicChannel;
static constexpr int mpdDedicChannel = 2;
static constexpr uint32_t mpdDedicMask = 1u << mpdDedicChannel;
#endif
