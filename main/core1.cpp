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
//#include <driver/gpio.h>
//#include <driver/dedic_gpio.h>
#include <xtensa_timer.h>
#include <xtensa_rtos.h>

//#include "driver/spi_master.h"
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
    //for(auto i : pins) gpio_ll_input_enable(NULL, i);
    //gpio_matrix_in(clockPin, CORE1_GPIO_IN0_IDX, false);

    while((dedic_gpio_cpu_ll_read_in()) == 0) {}
    while((dedic_gpio_cpu_ll_read_in()) != 0) {}
//    uint32_t lastTscFall = XTHAL_GET_CCOUNT(); 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}
  
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, extSel_Mask | mpdMask); 
    REG_WRITE(GPIO_OUT1_W1TS_REG, extSel_Mask | mpdMask); 

    RAM_VOLATILE uint8_t * const bankD800[2] = { &pbiROM[0], &atariRam[0xd800]};
    uint32_t r0 = 0, r1 = 0;

    while(true) {    
        while((dedic_gpio_cpu_ll_read_in()) != 0) {}
        uint32_t tscFall = XTHAL_GET_CCOUNT();
        int mpdSelect = ((bankD100Write[0xd1ff & bankOffsetMask] & pbiDeviceNumMask) ^ pbiDeviceNumMask) >> pbiDeviceNumShift;
        uint32_t setMask = (mpdSelect << mpdShift) | extSel_Mask;

        const uint32_t bmonTrace = (r0 << bmonR0Shift) | ((r1 & dataMask) >> dataShift);
        REG_WRITE(SYSTEM_CORE_1_CONTROL_1_REG, bmonTrace);

        // 4 nops
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");

	    REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinDisableMask);
        // Timing critical point #0: >= 14 ticks before the disabling the data lines 
        PROFILE0(XTHAL_GET_CCOUNT() - tscFall); 

        banks[(0xd800 >> bankShift) + BANKSEL_RD + BANKSEL_RAM] = bankD800[mpdSelect];
        banks[((0xd800 >> bankShift) + 1) + BANKSEL_RD + BANKSEL_RAM] = bankD800[mpdSelect] + bankSize;
        banks[(0xd800 >> bankShift) + BANKSEL_WR + BANKSEL_RAM] = bankD800[mpdSelect];
      
        // 9 nop minimum to fill the space b/w register io
        // 9 nops
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");
        __asm__ __volatile__ ("nop");

        // Timing critical point #0: >= 43 ticks after clock edge until read of address/control lines
        r0 = REG_READ(GPIO_IN_REG);
        PROFILE1(XTHAL_GET_CCOUNT() - tscFall); 

        const int bank = ((r0 & (readWriteMask | casInh_Mask | addrMask)) 
            >> (readWriteShift - bankBits - 1)); 

        if ((r0 & readWriteMask) != 0) {
            REG_WRITE(GPIO_ENABLE1_W1TS_REG, bankEnable[bank]);
            uint16_t addr = r0 >> addrShift;
            RAM_VOLATILE uint8_t *ramAddr = banks[bank] + (addr & bankOffsetMask);
            uint8_t data = *ramAddr;
            REG_WRITE(GPIO_OUT1_REG, (data << dataShift) | setMask);
            // Timing critical point #2: Data on bus by 85 ticks
            PROFILE2(XTHAL_GET_CCOUNT() - tscFall); 
            r1 = REG_READ(GPIO_IN1_REG);
            // Timing critical point #4: All work done by 111 ticks
            PROFILE4(XTHAL_GET_CCOUNT() - tscFall); 
    
        } else { //////////////// XXWRITE /////////////    
            uint16_t addr = r0 >> addrShift;
            RAM_VOLATILE uint8_t *ramAddr = banks[bank] + (addr & bankOffsetMask);
            while(XTHAL_GET_CCOUNT() - tscFall < 75) {}

            // Timing critical point #3: Wait at least 80 ticks before reading data lines 
            PROFILE3(XTHAL_GET_CCOUNT() - tscFall); 
            r1 = REG_READ(GPIO_IN1_REG); 
            uint8_t data = (r1 >> dataShift);
            *ramAddr = data;
            
            // Timing critical point #4: All work done by 111 ticks
            PROFILE5(XTHAL_GET_CCOUNT() - tscFall); 
        } 
    };
}
