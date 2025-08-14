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

#include "core1.h"

#pragma GCC optimize("O1")

void iloop_pbi() {
    uint32_t r0 = 0, r1 = 0;

    while((dedic_gpio_cpu_ll_read_in()) != 0) {} // sync with clock before starting loop 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}

    while(true) {    
        while((dedic_gpio_cpu_ll_read_in()) != 0) {} // wait for clock falling edge 
        uint32_t tscFall = XTHAL_GET_CCOUNT();
        bmonArray[bmonHead] = ((r0 << bmonR0Shift) | ((r1 & dataMask) >> dataShift)); // store last cycle's bus trace 
        bmonHead = (bmonHead + 1) & (bmonArraySz - 1);
	    REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinDisableMask);
        // Timing critical point #0: >= 14 ticks before the disabling the data lines 
        PROFILE0(XTHAL_GET_CCOUNT() - tscFall); 

        // 9 nop minimum to fill the space b/w REG_WRITE above and REG_READ below 
        // 9 nops
#if 1
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
#endif 

        // Timing critical point #0: >= 43 ticks after clock edge until read of address/control lines
        r0 = REG_READ(GPIO_IN_REG);
        PROFILE1(XTHAL_GET_CCOUNT() - tscFall); 

        const int bank = ((r0 & (readWriteMask | casInh_Mask | addrMask)) 
            >> (readWriteShift - bankBits - 1)); 

        if ((r0 & readWriteMask) != 0) {
            // BUS READ 
            REG_WRITE(GPIO_ENABLE1_W1TS_REG, bankEnable[bank] | pinEnableMask);
            uint16_t addr = r0 >> addrShift;
            RAM_VOLATILE uint8_t *ramAddr = banks[bank] + (addr & bankOffsetMask);
            uint8_t data = *ramAddr;
            REG_WRITE(GPIO_OUT1_REG, (data << dataShift) | extSel_Mask);
            // Timing critical point #2: Data output on bus by 85 ticks
            PROFILE2(XTHAL_GET_CCOUNT() - tscFall); 
            r1 = REG_READ(GPIO_IN1_REG);
            // Timing critical point #4: All work done by 120 ticks
            PROFILE4(XTHAL_GET_CCOUNT() - tscFall); 
    
        } else { 
            // BUS WRITE    
            uint16_t addr = r0 >> addrShift;
            RAM_VOLATILE uint8_t *ramAddr = banks[bank] + (addr & bankOffsetMask);
            while(XTHAL_GET_CCOUNT() - tscFall < 75) {}

            // Timing critical point #3: Wait at least 80 ticks before reading data lines 
            PROFILE3(XTHAL_GET_CCOUNT() - tscFall); 
            r1 = REG_READ(GPIO_IN1_REG); 
            uint8_t data = (r1 >> dataShift);
            *ramAddr = data;
            
            // Timing critical point #4: All work done by 120 ticks
            PROFILE5(XTHAL_GET_CCOUNT() - tscFall); 
        } 
    };
}
