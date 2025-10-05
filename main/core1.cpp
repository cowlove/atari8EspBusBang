#pragma GCC optimize("O1")
#include <esp_intr_alloc.h>
#include <rtc_wdt.h>
#include <esp_task_wdt.h>
#include <esp_async_memcpy.h>
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_reg.h"
#include <rtc_wdt.h>
#include <hal/cache_hal.h>
#include <hal/cache_ll.h>
#include <hal/dedic_gpio_cpu_ll.h>
#include <xtensa/core-macros.h>
#include <xtensa/hal.h>
#include <xtensa_timer.h>
#include <xtensa_rtos.h>

#include "rom/ets_sys.h"
#include "soc/dport_access.h"
#include "soc/system_reg.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_sig_map.h"
#include "hal/gpio_ll.h"
#include "rom/gpio.h"

#include "pinDefs.h"
#include "profile.h"
#include "main.h"
#include "util.h"

#pragma GCC optimize("O1")

void iloop_pbi() {
    uint32_t bmon = 0;
    uint8_t data = 0;
    int nextBmonHead = 1;

    while((dedic_gpio_cpu_ll_read_in()) != 0) {} // sync with clock before starting loop 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}

    while(true) {    
        while((dedic_gpio_cpu_ll_read_in()) != 0) {} // wait for clock falling edge 
        uint32_t tscFall = XTHAL_GET_CCOUNT();
        // Timing critical point #0: >= 14 ticks before the disabling the data lines (above) 
        PROFILE0(XTHAL_GET_CCOUNT() - tscFall); 
        
        // Store last cycle's bus trace data from previous loop r0 and r1  
        bmonArray[bmonHead] = bmon | data;
        bmonHead = nextBmonHead;
        
        // 9 ticks of wait state available here b/w REG_WRITE above and REG_READ below
        // Pre-fetch some volatiles into registers during this time  
        uint32_t pinEnMask = pinEnableMask;
        uint32_t pinDrMask = pinDriveMask;

        // Timing critical point #1: >= 43 ticks after clock edge until read of address/control lines
        //AsmNops<9>::generate(); // add <n> asm("nop;")
        PROFILE1(XTHAL_GET_CCOUNT() - tscFall); 
        uint32_t r0 = REG_READ(GPIO_IN_REG);

#define L1BANK_TIMING_TEST
#ifdef L1BANK_TIMING_TEST
        // !! *IF* this is right, L1 bank only adds about 3 cycles, from about 52 to 55 
        static const DRAM_ATTR uint32_t bankL1SelBits = (bus.rw.mask /*| bus.extDecode.mask*/ | bus.addr.mask);
        static const DRAM_ATTR uint32_t pageInBankSelBits = (bus.addr.mask & (bankL1OffsetMask << bus.addr.shift));
        static const DRAM_ATTR int bankL1SelShift = (bus.extDecode.shift - bankL1Bits - 1);
        static const DRAM_ATTR int pageSelShift = (bus.extDecode.shift - pageBits - 1);
        const int bankL1 = ((r0 & bankL1SelBits) >> bankL1SelShift);
        const int pageInBank = ((r0 & pageInBankSelBits) >> pageSelShift); 
        uint8_t *pageData = banks[bankL1]->pages[pageInBank];
        const uint32_t pageEn = banks[bankL1]->ctrl[pageInBank];
#else
        static const DRAM_ATTR uint32_t pageSelBits = (bus.rw.mask /*| bus.extDecode.mask*/ | bus.addr.mask);
        static const DRAM_ATTR int pageSelShift = (bus.extDecode.shift - pageBits - 1);
        const int page = ((r0 & pageSelBits) >> pageSelShift); 

        uint8_t *pageData = pages[page];
        const uint32_t pageEn = pageEnable[page];
        AsmNops<5>::generate(); // This is about what the L1BANK_TIMING_TEST difference was 
#endif

        REG_WRITE(GPIO_ENABLE1_W1TS_REG, (pageEn | pinDrMask) & pinEnMask);
        uint16_t addr = r0 >> bus.addr.shift;
        RAM_VOLATILE uint8_t *ramAddr = &pageData[addr & pageOffsetMask];
        data = *ramAddr;
        REG_WRITE(GPIO_OUT1_REG, (data << bus.data.shift));
        // Timing critical point #2: Data output on bus before ~60 ticks
        PROFILE2(XTHAL_GET_CCOUNT() - tscFall);
        
        bmon = (r0 << bmonR0Shift); // pre-compute part of bmon for saving at start of next cycle 
        nextBmonHead = (bmonHead + 1) & bmonArraySzMask;
        while(XTHAL_GET_CCOUNT() - tscFall < 77) {}

        PROFILE3(XTHAL_GET_CCOUNT() - tscFall);
        uint32_t r1 = REG_READ(GPIO_IN1_REG);
        if ((r0 & bus.rw.mask) == 0 && busWriteDisable == 0) {
            data = (r1 >> bus.data.shift);
            *ramAddr = data;
        }
        REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);

        // Timing critical point #4: All work done before ~120 ticks
        PROFILE4(XTHAL_GET_CCOUNT() - tscFall);     
    };
}
