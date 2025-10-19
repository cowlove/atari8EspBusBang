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
#include "mmu.h"
#include "cartridge.h"

//#pragma GCC optimize("O1")

static constexpr DRAM_ATTR int pageD5 = pageNr(0xd500) | PAGESEL_CPU | PAGESEL_WR;   // page to watch for cartidge control accesses
static constexpr DRAM_ATTR int bank80 = page2bank(pageNr(0x8000)); // bank to remap for cart control 
static constexpr DRAM_ATTR int bankC0 = page2bank(pageNr(0xc000)); // bank to remap for os rom enable bit 
//static constexpr DRAM_ATTR uint32_t bankL1SelBits = (bus.rw.mask /*| bus.extDecode.mask*/ | bus.addr.mask); // R0 mask for page+addr
static constexpr DRAM_ATTR uint32_t pageInBankSelBits = (bus.addr.mask & (bankL1OffsetMask << bus.addr.shift)); // R0 mask for page index within a bank
static constexpr DRAM_ATTR int bankL1SelShift = (bus.extDecode.shift - bankL1Bits - 1); // R0 shift to get bank number 
static constexpr DRAM_ATTR int pageSelShift = (bus.extDecode.shift - pageBits - 1);     // R0 shift to get page number 

DRAM_ATTR uint8_t lastPageOffset[nrPages * (1 << PAGESEL_EXTRA_BITS)] = {0}; // offset within page of last mem access, for each page 
//volatile DRAM_ATTR BankL1Entry *testbanks[pageSize] = {0};

void iloop_pbi() {
    uint32_t bmon = 0, r0 = 0;
    unsigned int nextBmonHead = 1;
    uint8_t data = 0;
    uint8_t dummyWrite;
    uint8_t *ramAddr = &dummyWrite;

    while((dedic_gpio_cpu_ll_read_in()) != 0) {} // sync with clock before starting loop 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}

    while(true) {    
        while((dedic_gpio_cpu_ll_read_in()) != 0) {} // wait for clock falling edge 
        //PROFILE_START();
        uint32_t tscFall = XTHAL_GET_CCOUNT();
        //nextBmonHead = (bHead + 1) & bmonArraySzMask;
        //bmonHead = nextBmonHead;
	int bHead = bmonHead;
        bmonArray[bHead] = bmon;       
        bmonHead = (bHead + 1) & bmonArraySzMask;
        uint32_t pinEnMask = pinEnableMask;
        uint32_t pinDrMask = pinDriveMask;
        AsmNops<0>::generate(); 

        // Timing critical point #1: >= 17 ticks after clock edge until read of address/control lines
        r0 = REG_READ(GPIO_IN_REG);
        PROFILE1(XTHAL_GET_CCOUNT() - tscFall); 

        const int bankL1 = ((r0 & bus.addr.mask) >> bankL1SelShift);
        const int pageInBank = ((r0 & pageInBankSelBits) >> pageSelShift)
               | ((r0 & bus.rw.mask) >> (bus.rw.shift - (pageBits - bankL1Bits)))
        ;  
        const uint32_t pageEn = banks[bankL1]->ctrl[pageInBank];
        uint8_t *pageData = banks[bankL1]->pages[pageInBank];
        
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, (pageEn | pinDrMask) & pinEnMask);
        uint16_t addr = r0 >> bus.addr.shift;
        ramAddr = &pageData[addr & pageOffsetMask];
        PROFILE2(XTHAL_GET_CCOUNT() - tscFall);

        const int isReadOp = ((r0 & bus.rw.mask) >> bus.rw.shift) | busWriteDisable;
        if (__builtin_expect(isReadOp, 1)) { 
                data = *ramAddr;
                REG_WRITE(GPIO_OUT1_REG, (data << bus.data.shift));
                bmon = (r0 << bmonR0Shift);
                bmon = bmon | data;
                // 20 nops with both lines enabled, 42 with none enabled, 24 with only basicEn enabled  
                banks[bank80] = basicEnBankMux[(d000Write[_0x301] >> 1) & 0x1];
                banks[bankC0] = osEnBankMux[d000Write[_0x301] & 0x1];
                //AsmNops<30>::generate(); 
                while(XTHAL_GET_CCOUNT() - tscFall < 105) {}

                REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
                PROFILE4(XTHAL_GET_CCOUNT() - tscFall);// 112-120 cycles seems to be the limits  // 
        } else {
                uint8_t pageOffset = addr & 0xff;
                lastPageOffset[pageInBank] = addr;
                basicEnBankMux[1] = cartBanks[lastPageOffset[pageD5]]; // remap bank 0xa000 
                banks[bank80] = basicEnBankMux[(d000Write[_0x301] >> 1) & 0x1];
                bmon = (r0 << bmonR0Shift);
                AsmNops<0>::generate(); 
                 
                while(XTHAL_GET_CCOUNT() - tscFall < 78) {}
                uint32_t r1 = REG_READ(GPIO_IN1_REG);
                PROFILE3(XTHAL_GET_CCOUNT() - tscFall);
                data = (r1 >> bus.data.shift);
                //uint8_t *writeMux = {ramAddr, &dummyWrite);}
                //*writeMux[busWriteDisable] = data;
                *ramAddr = data;
                bmon = bmon | data;
                while(XTHAL_GET_CCOUNT() - tscFall < 105) {}
                REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
                PROFILE5(XTHAL_GET_CCOUNT() - tscFall);     
        }
    }
}
