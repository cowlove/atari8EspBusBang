#pragma GCC optimize("Ofast")
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
#include "mmu.h"
#include "cartridge.h"
#include "hwType.h"

//static constexpr DRAM_ATTR uint32_t l1BlockSelBits = (bus.rw.mask /*| bus.extDecode.mask*/ | bus.addr.mask); // R0 mask for page+addr
static constexpr DRAM_ATTR uint32_t pageInL1BlockSelBits = (bus.addr.mask & (l1BlockOffsetMask << bus.addr.shift)); // R0 mask for page index within a block
static constexpr DRAM_ATTR int l1BlockSelShift = (bus.extDecode.shift - l1BlockBits - 1); // R0 shift to get block number
static constexpr DRAM_ATTR int pageSelShift = (bus.extDecode.shift - pageBits - 1);     // R0 shift to get page number 

DRAM_ATTR uint8_t lastPageOffset[nrPages * (1 << PAGESEL_EXTRA_BITS)] = {0}; // offset within page of last mem access, for each page 

template <CartBankSelect BankSelect>
__attribute__((section(".text.iloop_pbi"), noinline, used))
void iloop_pbi() {
    uint32_t bmon = 0, r0 = 0;
    unsigned int nextBmonHead = 1;
    uint8_t data = 0;
    uint8_t dummyWrite;
    uint8_t *ramAddr = &dummyWrite;

    while((dedic_gpio_cpu_ll_read_in()) != 0) {} // sync with clock before starting loop 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}

    while(true) {    
        PROFILE_GPIO_PIN_CLEAR();
        while((dedic_gpio_cpu_ll_read_in()) != 0) {
            if (dedic_gpio_cpu_ll_read_in() == 0) break;
            if (dedic_gpio_cpu_ll_read_in() == 0) break;
            if (dedic_gpio_cpu_ll_read_in() == 0) break;
            if (dedic_gpio_cpu_ll_read_in() == 0) break;
            if (dedic_gpio_cpu_ll_read_in() == 0) break;
            if (dedic_gpio_cpu_ll_read_in() == 0) break;
            if (dedic_gpio_cpu_ll_read_in() == 0) break;
            if (dedic_gpio_cpu_ll_read_in() == 0) break;
        } // wait for clock falling edge 

        PROFILE_LOOP_START();
        int bHead = bmonHead;
        bmonArray[bHead].r0 = r0;       
        bmonHead = (bHead + 1) & bmonArraySzMask;
        AsmNops<LEADIN_NOPS>::generate(); 

        // Timing critical point #1: >= 17 ticks after clock edge until read of address/control lines
        r0 = REG_READ(GPIO_IN_REG);
        PROFILE1();

        const int l1BlockIndex = ((r0 & bus.addr.mask) >> l1BlockSelShift);
        const int pageInL1Block = ((r0 & pageInL1BlockSelBits) >> pageSelShift)
              | ((r0 & bus.rw.mask) >> (bus.rw.shift - (pageBits - l1BlockBits)));
        // possibly remove this shift  ^^^ statement if RW pin is moved
          
        const uint32_t pageEn = mmuState.l1Blocks[l1BlockIndex]->ctrl[pageInL1Block];
        uint8_t *pageData = mmuState.l1Blocks[l1BlockIndex]->pages[pageInL1Block];
        
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, pageEn); 
        PROFILE2();
        uint16_t addr = r0 >> bus.addr.shift;
        ramAddr = &pageData[addr & pageOffsetMask];

        const int isReadOp = ((r0 & bus.rw.mask) >> bus.rw.shift);
        if (__builtin_expect(isReadOp, 1)) { 
            data = *ramAddr;
            REG_WRITE(GPIO_OUT1_REG, (data << bus.data.shift));
            PROFILE3();
            
            // Update BASIC ROM and PBI ROM memory mapping based on d000Write control bits
            mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
            int bankC0Select = (d000Write[_0x301] & 0x1) | ((d000Write[_0x1ff] & pbiDeviceNumMask));
            mmuState.l1Blocks[l1BlockC0] = mmuState.osEnL1BlockMux[bankC0Select];

            // Update extended mem bank and basic ROM mapping
            const int portb = d000Write[_0x301];
            const int extMemBank = (portb & 0x7c) >> 2;
            mmuState.l1Blocks[l1Block40] = mmuState.extBanks[extMemBank];
            
            loopNezDelay(RD_EXIT_DELAY); 
            REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
            PROFILE4(); // 112-120 cycles seems to be the limits
        } else {
            const int page = pageNr(addr);
            lastPageOffset[page] = addr;

            // Update cartridge banking based on d500 write address or data, depending on scheme
            const int cartBankIndex =
                (BankSelect == CartBankSelect::D500_ADDR_BANK_SEL)
                    ? (lastPageOffset[pageD5] & 0x1f)
                    : (d000Write[_0x500] & 0x1f);
            mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];

            // Update extended mem bank and basic ROM mapping
            mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
            
            loopNezDelay(WR_EXIT_DELAY); 
            uint32_t r1 = REG_READ(GPIO_IN1_REG);
            PROFILE3();
            REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
            data = (r1 >> bus.data.shift);
            *ramAddr = data;
            PROFILE5();
        }
    }
}

template void iloop_pbi<CartBankSelect::D500_ADDR_BANK_SEL>();
template void iloop_pbi<CartBankSelect::D500_DATA_BANK_SEL>();
