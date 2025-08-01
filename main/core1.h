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

#define PROFILE1(a) {}
#define PROFILE2(a) {}
#define PROFILE3(a) {}
#define PROFILE4(a) {}
#define PROFILE5(a) {}

#ifdef PROFA
#define PROFILE1(ticks) profilers[0].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROFB
#define PROFILE2(ticks) profilers[1].add(ticks)
#define PROFILE3(ticks) profilers[2].add(ticks)
#define FAKE_CLOCK
#endif
#ifdef PROFC
#define PROFILE4(ticks) profilers[1].add(ticks)
#define PROFILE5(ticks) profilers[2].add(ticks)
#define FAKE_CLOCK
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

static const struct {
#ifdef FAKE_CLOCK
   bool fakeClock     = 1; 
   float histRunSec   = TEST_SEC;
#else 
   bool fakeClock     = 0;
   float histRunSec   = 7200;
#endif 
   bool testPins      = 0;
   bool watchPins     = 0;      // loop forever printing pin values w/ INPUT_PULLUP
   bool dumpSram      = 0;   ;
   bool timingTest    = 0;
   bool bitResponse   = 0;
   bool core0Led      = 0; // broken, PBI loop overwrites entire OUT1 register including 
   int dumpPsram      = 999999;
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
static const int      casInh_pin = 17;
static const int      casInh_Shift = casInh_pin;
static const int      casInh_Mask = (0x1 << casInh_pin);               // pin 0 
static const int      clockPin = 0;
//static const int      clockMask = (0x1 << clockPin);
static const int      addr0Pin = 1;
static const int      addrShift = addr0Pin;                   // bus address - pins 1-16
static const int      addrMask = 0xffff << addrShift;  // 
static const int      refreshPin = 21;
static const int      refreshMask = (1 << refreshPin);
static const int      readWritePin = 18;
static const int      readWriteShift = readWritePin;
static const int      readWriteMask = (1 << readWritePin); 

//GPIO1 pins
static const int      interruptPin = 48;
static const int      interruptShift = (interruptPin & 31);
static const int      interruptMask = 1 << interruptShift; 

#ifdef HAVE_RESET_PIN
static const int      resetPin = 46;
static const int      resetMask = 1 << (resetPin - 32); 
#endif
static const int      mpdPin = 46;  // active low
static const int      mpdShift = (mpdPin - 32);
static const int      mpdMask = 1 << mpdShift; 
static const int      extSel_Pin = 47; // active high 
static const int      extSel_PortPin = extSel_Pin - 32 /* port1 pin*/;
static const int      extSel_Mask = (1 << extSel_PortPin);
static const int      data0Pin = 38;
static const int      data0Mask = (data0Pin - 32);
static const int      data0PortPin = data0Pin - 32;
static const int      dataShift = data0PortPin;
static const int      dataMask = (0xff << dataShift);

#ifdef HAVE_RESET_PIN
static const uint32_t copyResetMask = 0x40000000;
#endif
static const uint32_t copyMpdMask = 0x40000000;
static const uint32_t copyDataShift = 22;
static const uint32_t copyDataMask = 0xff << copyDataShift;

// TODO: try pin 19,20 (USB d- d+ pins). Move reset to 0 so ESP32 boot doesnt get messed up by low signal   
// TODO: maybe eventually need to drive PBI interrupt pin 
// TODO: so eventaully looks like: pin 0 = reset, pin 19 = casInh input, pin 20 = interrupt, pin 47 = MPD
// TODO: although USB pins moving during ESP32 boot might cause conflict 
// TODO: extend this generally, need to review which ESP32 pins are driven during boot or have strapping resistors   
// TODO: can we move all the address lines down by one pin to allow R/W to be the newest high
//    bit in a bigger bank index?
using std::vector;
static const vector<int> pins = {
//
// +--Clock                                    +-- CasInhAL                   +--MPD out
// | +---Refresh                               |  +-- Read                    |  +-- ext sel out
// | | +--- ADDR                               |  |   +---DATA                |  |  +- Interrupt out
// V V V + + + + + + + +  +  +  +  +  +  +  +  V  V   V  +  +  +  +  +  +  +  V  V  V
   0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21, 38,39,40,41,42,43,44,45,46,47,48};
//static const int led_NO_Pin = -1;

static const int bankBits = 8;
static const int nrBanks = 1 << bankBits;
static const int bankSize = 64 * 1024 / nrBanks;
static const uint16_t bankOffsetMask = bankSize - 1;
static const uint16_t bankMask = ~bankOffsetMask;
static const int bankShift = 16 - bankBits;

static const int BANKSEL_RD = (1 << (bankBits + 1));
static const int BANKSEL_WR = 0;
static const int BANKSEL_RAM = (1 << bankBits);
static const int BANKSEL_ROM = 0;

#define BUSCTL_VOLATILE //volatile
#define RAM_VOLATILE //volatile

extern DRAM_ATTR RAM_VOLATILE uint8_t *banks[nrBanks * 4];
extern DRAM_ATTR uint32_t bankEnable[nrBanks * 4];
extern DRAM_ATTR RAM_VOLATILE uint8_t atariRam[64 * 1024];
extern DRAM_ATTR RAM_VOLATILE uint8_t atariRomWrites[64 * 1024];
extern DRAM_ATTR RAM_VOLATILE uint8_t cartROM[];
extern DRAM_ATTR RAM_VOLATILE uint8_t pbiROM[2 * 1024];
extern DRAM_ATTR RAM_VOLATILE uint8_t bankD100Write[bankSize];
extern DRAM_ATTR RAM_VOLATILE uint8_t bankD100Read[bankSize];

extern BUSCTL_VOLATILE uint32_t busMask;

struct Hist2 { 
    static const int maxBucket = 512; // must be power of 2
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

static const int numProfilers = 4;
extern DRAM_ATTR Hist2 profilers[numProfilers];

#if 1
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

#define PDIMSK 0x249

//static const int pbiDeviceNum = ; // also change in pbirom.asm
static const int pbiDeviceNumMask = 0x2;
static const int pbiDeviceNumShift = 1;

static const int bmonR0Shift = 8;

#ifndef ARDUINO
static inline void delay(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static inline void yield() { delay(0); }
static inline void digitalWrite(int pin, int val) {
   gpio_set_level((gpio_num_t)pin, val);
}
static inline int digitalRead(int pin) { return gpio_get_level((gpio_num_t)pin); }
static inline void disableCore0WDT() {}
static inline void enableCore0WDT() {}
static inline unsigned long millis() { 
     return (unsigned long)(esp_timer_get_time() / 1000ULL);
}
#define GIT_VERSION "git-version"
struct ArduinoSerial {
   bool available() { return false; }
   int read() { return 0; }
};
extern ArduinoSerial Serial;

#include "hal/gpio_hal.h"
#include "soc/soc_caps.h"
#include <cstring>
#include "hal/ledc_types.h"
#include "driver/ledc.h"

#define INPUT 0x01
#define OUTPUT            0x03
#define PULLUP            0x04
#define INPUT_PULLUP      0x05
#define PULLDOWN          0x08
#define INPUT_PULLDOWN    0x09
#define OPEN_DRAIN        0x10
#define OUTPUT_OPEN_DRAIN 0x13
#define ANALOG            0xC0

#define log_w printf
#define log_e printf
#define log_i printf

static inline void pinMode(uint8_t p, uint8_t m) {
   gpio_num_t pin = (gpio_num_t)p;
   gpio_mode_t mode = (gpio_mode_t)m;
   gpio_hal_context_t gpiohal;
   gpiohal.dev = GPIO_LL_GET_HW(GPIO_PORT_0);

   gpio_config_t conf = {
      .pin_bit_mask = (1ULL << pin),              /*!< GPIO pin: set with bit mask, each bit maps to a GPIO */
      .mode = GPIO_MODE_DISABLE,                  /*!< GPIO mode: set input/output mode                     */
      .pull_up_en = GPIO_PULLUP_DISABLE,          /*!< GPIO pull-up                                         */
      .pull_down_en = GPIO_PULLDOWN_DISABLE,      /*!< GPIO pull-down                                       */
      .intr_type = (gpio_int_type_t)gpiohal.dev->pin[pin].int_type /*!< GPIO interrupt type - previously set                 */
   };
   if (mode < 0x20) {  //io
      conf.mode = (gpio_mode_t)(mode & (INPUT | OUTPUT));
      if (mode & OPEN_DRAIN) {
         conf.mode = (gpio_mode_t)(conf.mode | GPIO_MODE_DEF_OD);
      }
      if (mode & PULLUP) {
         conf.pull_up_en = GPIO_PULLUP_ENABLE;
      }
      if (mode & PULLDOWN) {
         conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
      }
   }
   if (gpio_config(&conf) != ESP_OK) {
      log_e("IO %i config failed", pin);
      return;
   }
}

static inline bool ledcAttachChannel(uint8_t pin, uint32_t freq, uint8_t resolution, uint8_t channel) {
   ledc_timer_config_t ledc_timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = (ledc_timer_bit_t)resolution,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = freq,
      .clk_cfg = LEDC_AUTO_CLK,
   };
   ledc_timer_config(&ledc_timer);

   ledc_channel_config_t ledc_channel = {0};
   ledc_channel.channel = (ledc_channel_t)channel;
   ledc_channel.duty = 0;
   ledc_channel.gpio_num = pin;
   ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
   ledc_channel.timer_sel = LEDC_TIMER_0;
   ledc_channel_config(&ledc_channel);
   return true;
}

static inline void ledcWrite(int channel, int duty) {
   ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, duty);
   ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel);
}

#endif
