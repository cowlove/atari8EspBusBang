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
#include "core1.h"

#pragma GCC optimize("O1")
void NEWneopixelWrite(uint8_t, uint8_t, uint8_t);
void iloop_pbi() {
    uint32_t r0 = 0, r1 = 0;

    while((dedic_gpio_cpu_ll_read_in()) != 0) {} // sync with clock before starting loop 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}

    while(true) {    
        while((dedic_gpio_cpu_ll_read_in()) != 0) {} // wait for clock falling edge 
        uint32_t tscFall = XTHAL_GET_CCOUNT();
        
        // Store last cycle's bus trace data from previous loop r0 and r1  
        bmonArray[bmonHead] = ((r0 << bmonR0Shift) | ((r1 & bus.data.mask) >> bus.data.shift)); 
        bmonHead = (bmonHead + 1) & bmonArraySzMask;
        
        // Timing critical point #0: >= 14 ticks before the disabling the data lines (above) 
        PROFILE0(XTHAL_GET_CCOUNT() - tscFall); 

        // 9 ticks of wait state available here b/w REG_WRITE above and REG_READ below
        // Pre-fetch some volatiles into registers during this time  
        uint32_t pinEnMask = pinEnableMask;
        uint32_t pinDrMask = pinDriveMask;

        // Timing critical point #1: >= 43 ticks after clock edge until read of address/control lines
        ASM("nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop");
        r0 = REG_READ(GPIO_IN_REG);
        PROFILE1(XTHAL_GET_CCOUNT() - tscFall); 

        static const DRAM_ATTR uint32_t pageSelBits = (bus.rw.mask /*| bus.extDecode.mask*/ | bus.addr.mask);
        static const DRAM_ATTR int pageSelShift = (bus.extDecode.shift - pageBits - 1);
        const int page = ((r0 & pageSelBits) >> pageSelShift); 

        REG_WRITE(GPIO_ENABLE1_W1TS_REG, (pageEnable[page] | pinDrMask) & pinEnMask);
        uint16_t addr = r0 >> bus.addr.shift;
        RAM_VOLATILE uint8_t *ramAddr = pages[page] + (addr & pageOffsetMask);
        uint8_t data = *ramAddr;
        REG_WRITE(GPIO_OUT1_REG, (data << bus.data.shift));
        // Timing critical point #2: Data output on bus before ~95 ticks
        PROFILE2(XTHAL_GET_CCOUNT() - tscFall);

        while(XTHAL_GET_CCOUNT() - tscFall < 75) {}

        PROFILE3(XTHAL_GET_CCOUNT() - tscFall);
        r1 = REG_READ(GPIO_IN1_REG);
        if ((r0 & bus.rw.mask) == 0 && busWriteDisable == 0) {
            data = (r1 >> bus.data.shift);
            *ramAddr = data;
        }

	    REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
        // Timing critical point #4: All work done before ~120 ticks
        PROFILE4(XTHAL_GET_CCOUNT() - tscFall);     
    };
}
