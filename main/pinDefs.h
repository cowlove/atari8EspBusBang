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
// Every bus GPIO, for setup loops. A constexpr array, not a std::vector: no
// static constructor or heap copy in each of the files that include this.
static constexpr int gpios[] = {
// GPIO0 PINS:
//
// +--Audio/Unused (carrier: link TX)        +-- espBusBang: A14 A15 R/W;
// | +--- ADDR 0-13                          |   carrier:    R/W A14 A15
// | |                                       |        +-- Sidecar link RX (was READY)
// | |                                       |        |  +-- Refresh
// | |                                       |        |  | 
   0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,      15,16,17,18,21,

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

// The Atari refresh_ input: CONFIG_A8BB_REFRESH_GPIO (Kconfig), GPIO21 in
// every build (Sidecar builds too: their link RX is GPIO18 now); 0 = none.
// 19 (main's USB D-, the pad turned off at boot) was the Sidecar builds'
// spot while the link RX had GPIO21, and still works if set. HAVE_REFRESH
// gates every use of bus.refresh_. -DNO_REFRESH=1 (bench) drops refresh_ in
// any build.
#if CONFIG_A8BB_REFRESH_GPIO > 0 && !defined(NO_REFRESH)
#define HAVE_REFRESH 1
#endif
// GPIO19_IN_USE: main uses GPIO19 (USB D-), so its USB-Serial-JTAG pad is
// turned off at boot and the USJ driver is never installed: IDF's
// usb_serial_jtag_driver_install() turns the pads back on (found on .6:
// the pin never received). No USB console input then (handleSerialInput()).
#if defined(HAVE_REFRESH) && CONFIG_A8BB_REFRESH_GPIO == 19
#define GPIO19_IN_USE 1
#endif
#if A8BB_SIDECAR && CONFIG_A8BB_REFRESH_GPIO == 18
#error "refresh_ on GPIO18 collides with the Sidecar link's RX: A8BB_REFRESH_GPIO"
#endif

// The main board (Kconfig A8BB_MAIN_BOARD): where R/W and A14-A15 sit.
// espBusBang (the test pool): A0-A15 on GPIO1-16, R/W on GPIO17. The DevKitC
// carrier and sidecar_pcb_xl: R/W on GPIO15, between A13 and A14, so it sits
// right above A8-A13 and core 1 takes the page's table index (page in block,
// PAGESEL_RD) with one extract (core1.cpp). The address is addrLo (A0-A13)
// and addrHi (A14-A15): use r0ToAddr()/addrToR0() (below), not a shift.
#if CONFIG_A8BB_BOARD_CARRIER
#define A8BB_BOARD_NAME "carrier"
#else
#define A8BB_BOARD_NAME "espBusBang"
#endif

static const DRAM_ATTR struct BusPins {
   // GPIO0 pins, primarily input 
   static const Pin<36>     clock; 
   static const Pin<1,14>  addrLo;   // A0-A13
#if CONFIG_A8BB_BOARD_CARRIER
   static const Pin<15>    rw;
   static const Pin<16,2>  addrHi;   // A14-A15
#else
   static const Pin<15,2>  addrHi;   // A14-A15
   static const Pin<17>    rw;
#endif
   // GPIO18 was READY (extDecode, never used): the Sidecar link's RX now.
#ifdef HAVE_REFRESH
   static const Pin<CONFIG_A8BB_REFRESH_GPIO> refresh_; // active low 
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

// The address lines in r0 (GPIO_IN), and conversions both ways. constexpr,
// so pre-shifted constants (bmon triggers, csim) cost nothing. On
// espBusBang addrHi follows addrLo, so r0ToAddr() is one shift and mask.
static constexpr uint32_t r0AddrMask = uint32_t(bus.addrLo.mask) | uint32_t(bus.addrHi.mask);
static constexpr bool addrContiguous = bus.addrHi.shift == bus.addrLo.shift + bus.addrLo.bits;
static constexpr __attribute__((always_inline)) inline uint16_t r0ToAddr(uint32_t r0) {
    if constexpr (addrContiguous)
        return uint16_t((r0 & r0AddrMask) >> bus.addrLo.shift);
    else
        return uint16_t(((r0 & bus.addrLo.mask) >> bus.addrLo.shift)
                        | (((r0 & bus.addrHi.mask) >> bus.addrHi.shift) << bus.addrLo.bits));
}
static constexpr __attribute__((always_inline)) inline uint32_t addrToR0(uint16_t addr) {
    return ((uint32_t(addr) & ((1U << bus.addrLo.bits) - 1)) << bus.addrLo.shift)
           | ((uint32_t(addr) >> bus.addrLo.bits) << bus.addrHi.shift);
}
// The GPIO of address bit n (0-15), for setup loops.
static constexpr inline int addrBitPin(int n) {
    return n < bus.addrLo.bits ? bus.addrLo.pin + n : bus.addrHi.pin + (n - bus.addrLo.bits);
}
static_assert(bus.addrLo.bits + bus.addrHi.bits == 16, "16 address lines");
static_assert(r0ToAddr(addrToR0(0xd5b9)) == 0xd5b9 && r0ToAddr(addrToR0(0x4000)) == 0x4000,
              "r0ToAddr/addrToR0 round trip");
static_assert((r0AddrMask & bus.rw.mask) == 0, "R/W isn't an address line");

// GPIO48 is shared: the status LED when free; the profiling pin core 1
// drives in PROFILE_MODE=PIN (PROFILE_GPIO_PIN_OUTPUT); or the Sidecar link's
// BCLK (A8BB_SIDECAR). PROFILE_MODE=GPIO only samples it as an input
// (the LED stays off so nothing drives it), so it works with the Sidecar.
// LEGACY profiling doesn't use it.
#if A8BB_SIDECAR && defined(PROFILE_GPIO_PIN_OUTPUT)
#error "PROFILE_MODE=PIN (drives GPIO48) can't be used with A8BB_SIDECAR"
#endif
#if A8BB_SIDECAR || defined(PROFILE_GPIO_SAMPLE)
DRAM_ATTR static const int ledPin = -1;
#define HAS_STATUS_LED 0
#else
DRAM_ATTR static const int ledPin = 48;
#define HAS_STATUS_LED 1
#endif
DRAM_ATTR static const int profPin = 48;

#if A8BB_SIDECAR
// Host pins for the Sidecar link (see main/sidecarHost.h). BCLK takes GPIO48
// (above). CSIM wires virtual numbers to the Sidecar's pins instead.
//
// Hardware: host GPIO0 (TX) -> Sidecar GPIO9, Sidecar GPIO46 -> host GPIO18
// (RX), host GPIO48 BCLK -> Sidecar GPIO10; the host drives BCLK (pin
// choices: sidecar/main/sidecarApp.cpp). The carrier and XL boards hard-wire
// RX to GPIO18; on the test pool's espBusBang boards GPIO18 is a READY input
// nothing in the firmware uses (the Atari side must not drive it). TX:
// - GPIO0 (TX) is routed to the PBI bus audio input, and is a strapping pin.
//   The line's loading keeps loopback from ever locking on at least one test
//   pool board (miner6 board 5). Future revisions isolate GPIO0; any Atari
//   audio input would come from the Sidecar, which has free pins.
static const int sidecarBclkPin = 48;
#ifdef CSIM
static const int sidecarTxPin = 100, sidecarRxPin = 101;
#else
static const int sidecarTxPin = 0, sidecarRxPin = 18;
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
