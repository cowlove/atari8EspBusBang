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
#include <driver/gpio.h>
#include <driver/dedic_gpio.h>
#include <freertos/xtensa_timer.h>
#include <freertos/xtensa_rtos.h>

#include "driver/spi_master.h"
#include "rom/ets_sys.h"
#include "soc/dport_access.h"
#include "soc/system_reg.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_sig_map.h"
#include "hal/gpio_ll.h"
#include "rom/gpio.h"

#include "core1.h"

#pragma GCC optimize("O3")

//SYSTEM_CORE_1_CONTROL_0_REG
#define RPACK(r0, data) ((r0 & 0x3fffff) | (data << 24))

//static uint8_t dummyWriteBank[bankSize];

void IRAM_ATTR __attribute__((optimize("O1"))) iloop_pbi() {
    for(int i = 0; i < nrBanks; i++) {
        banks[i] = &atariRomWrites[64 * 1024 / nrBanks * i];
        banks[i + nrBanks] = &atariRam[64 * 1024 / nrBanks * i];
    };
    for(int i = 0; i < nrBanks; i++) {
        bankEnable[i] = mpdMask | extSel_Mask; 
        bankEnable[i + nrBanks] = dataMask | mpdMask | extSel_Mask;
    };
    //atariRam[0xd803] = 0x00;

    uint8_t dummyWrite;
    RAM_VOLATILE uint8_t *prevWriteLoc = &dummyWrite;
    uint32_t prevWriteR1 = 0;

    for(auto i : pins) gpio_ll_input_enable(NULL, i);
    gpio_matrix_in(clockPin,      CORE1_GPIO_IN0_IDX, false);

    while((dedic_gpio_cpu_ll_read_in()) == 0) {}
    while((dedic_gpio_cpu_ll_read_in()) != 0) {}
//    uint32_t lastTscFall = XTHAL_GET_CCOUNT(); 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}
  
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, extSel_Mask | mpdMask); 
    REG_WRITE(GPIO_OUT1_W1TS_REG, extSel_Mask | mpdMask); 

    RAM_VOLATILE uint8_t * const bankD800[2] = { &pbiROM[0], &atariRam[0xd800]};
 
    do {    
        //while((dedic_gpio_cpu_ll_read_in() & dedicClockMask) != 0) {}
        while((dedic_gpio_cpu_ll_read_in()) != 0) {}
        uint32_t tscFall = XTHAL_GET_CCOUNT();
        int mpdSelect = (atariRomWrites[0xd1ff] & 1) ^ 1;
        banks[(0xd800 >> bankShift) + nrBanks] = bankD800[mpdSelect];
        uint32_t setMask = (mpdSelect << mpdShift) | busMask;
        //__asm__ __volatile__ ("nop");

        // Timing critical point #1: >= 4 ticks before the disabling the data lines 
        PROFILE1(XTHAL_GET_CCOUNT() - tscFall); 
        REG_WRITE(GPIO_ENABLE1_W1TC_REG, dataMask | extSel_Mask);
        uint32_t r0 = REG_READ(GPIO_IN_REG);
        // TODO: we could rearrange the address pins with casInh_pin directly above
        // addrPin15 so that we could just mask and shift r0 instead of using dedic_
        // to remap the order for us.  Looks like it would save a cycle 
        int bank = (r0 & (casInh_Mask | addrMask)) >> (casInh_Shift - 5); // timing approximateion
        const uint32_t pinEnableMask = bankEnable[bank];
        
        if ((r0 & (readWriteMask)) != 0) {
            uint16_t addr = (r0 & addrMask) >> addrShift;
            RAM_VOLATILE uint8_t *ramAddr = banks[bank] + (addr & ~bankMask);
            uint8_t data = *ramAddr;
            REG_WRITE(GPIO_ENABLE1_W1TS_REG, pinEnableMask);
            REG_WRITE(GPIO_OUT1_REG, (data << dataShift) | setMask);

            // Timing critical point #2 - REG_WRITE completed by 85 ticks
            PROFILE2(XTHAL_GET_CCOUNT() - tscFall); 
            //REG_WRITE(SYSTEM_CORE_1_CONTROL_1_REG, r0);

            *prevWriteLoc = (prevWriteR1 & dataMask) >> dataShift;
            prevWriteLoc = &dummyWrite;
            while((dedic_gpio_cpu_ll_read_in()) == 0) {}

            // Timing critical point #4:  All work done by 111 ticks
            PROFILE4(XTHAL_GET_CCOUNT() - tscFall); 
    
        } else { //////////////// XXWRITE /////////////    
            uint16_t addr = (r0 & addrMask) >> addrShift; 
            prevWriteLoc = banks[bank] + (addr & ~bankMask);

            //while((dedic_gpio_cpu_ll_read_in()) == 0) {}
            //__asm__ __volatile__ ("nop");
            //__asm__ __volatile__ ("nop");

            //banks[(0xd800 >> bankShift) + nrBanks] = bankD800[mpdSelect];
            //REG_WRITE(SYSTEM_CORE_1_CONTROL_1_REG, r0); // 6-7 cycles
            while(XTHAL_GET_CCOUNT() - tscFall < 85) {}

            // Timing critical point #3: Wait at least 80 ticks before reading data lines 
            PROFILE3(XTHAL_GET_CCOUNT() - tscFall); 
            prevWriteR1 = REG_READ(GPIO_IN1_REG); 
            
            // Timing critical point #4:  All work done by 111 ticks
            PROFILE5(XTHAL_GET_CCOUNT() - tscFall); 
        } 
    } while(1);
}
