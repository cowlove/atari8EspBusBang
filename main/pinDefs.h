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
   // No pads: their GPIO_ENABLE1 bits stand for extSel_ and mpd_ in the
   // MMU's ctrl[] words (see below), which core 1 also writes to its
   // dedicated outputs.
   static const Pin<33>    ctrlExtSel;
   static const Pin<34>    ctrlMpd;
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

// extSel_ and mpd_ come from core 1's dedicated GPIO outputs (channels
// bus.ctrlExtSel.shift and bus.ctrlMpd.shift, i.e. 1 and 2), not from their
// GPIO_ENABLE1 bits like the other bus pins: an APB store's pin edge lands
// ~25 ticks after it, and both lines are sampled at phi2's rising edge
// (30-50 ns setup). The pads are open drain with their output enables left
// on, and the GPIO matrix inverts the channels: 1 pulls the line low
// (asserted), 0 leaves it to the pull-up. Core 0's setup is in main.cpp;
// core 1 drives them in core1.cpp. Channel 0 is the profile pin.
//
// In an L1Block::ctrl[] word (the pin-enable mask core 1 writes to
// GPIO_ENABLE1_W1TS each cycle) they are bus.ctrlExtSel and bus.ctrlMpd:
// GPIO33's and GPIO34's enable bits, not the pins' own. Those pins have no
// pads on the ESP32-S3 (and the QSPI PSRAM doesn't use them), so enabling
// them does nothing, and their ENABLE1 bits 1 and 2 are exactly the two
// channels. Core 1 asserts both lines with one ee.wr_mask_gpio_out of the
// ctrl word as value and mask: it sets the channels whose bits are set and
// leaves the rest. The word's other bits only reach channels routed to no
// pin (halt_: 3, D0/D1: 6/7); bit 0 (channel 0, the profile pin) is
// GPIO32's, never used.
static_assert(bus.ctrlExtSel.shift == 1 && bus.ctrlMpd.shift == 2,
              "extSel_'s and mpd_'s ctrl bits must be dedicated channels 1 and 2");
static_assert(((bus.data.mask | bus.halt_.mask | bus.irq_.mask)
               & (1 | bus.ctrlExtSel.mask | bus.ctrlMpd.mask)) == 0,
              "a ctrl bit would reach the profile pin's or extSel_/mpd_'s channel");
