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

#pragma GCC optimize("O1")

static const DRAM_ATTR int pageD5 = pageNr(0xd500) | PAGESEL_CPU | PAGESEL_WR;   // page to watch for cartidge control accesses
static const DRAM_ATTR int bankA0 = page2bank(pageNr(0xa000) | PAGESEL_CPU | PAGESEL_RD); // bank to remap for cart control 
static const DRAM_ATTR uint32_t bankL1SelBits = (bus.rw.mask /*| bus.extDecode.mask*/ | bus.addr.mask); // R0 mask for page+addr
static const DRAM_ATTR uint32_t pageInBankSelBits = (bus.addr.mask & (bankL1OffsetMask << bus.addr.shift)); // R0 mask for page index within a bank
static const DRAM_ATTR int bankL1SelShift = (bus.extDecode.shift - bankL1Bits - 1); // R0 shift to get bank number 
static const DRAM_ATTR int pageSelShift = (bus.extDecode.shift - pageBits - 1);     // R0 shift to get page number 

DRAM_ATTR uint8_t lastPageOffset[nrPages * (1 << PAGESEL_EXTRA_BITS)] = {0}; // offset within page of last mem access, for each page 
//volatile DRAM_ATTR BankL1Entry *testbanks[pageSize] = {0};

void iloop_pbi() {
    uint32_t bmon = 0, r0 = 0;
    unsigned int nextBmonHead = 1;
    uint8_t data = 0;
    uint8_t dummyWrite;
    uint8_t *ramAddr = 0;

    while((dedic_gpio_cpu_ll_read_in()) != 0) {} // sync with clock before starting loop 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}

    while(true) {    
        while((dedic_gpio_cpu_ll_read_in()) != 0) {} // wait for clock falling edge 
        PROFILE_START();
        //uint32_t tscFall = XTHAL_GET_CCOUNT();
        REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
        AsmNops<0>::generate(); 
        bmon = bmon | data;
        bmonArray[bmonHead] = bmon;       
        bmonHead = nextBmonHead;

        uint32_t pinEnMask = pinEnableMask;
        uint32_t pinDrMask = pinDriveMask;

        //AsmNops<0>::generate(); 
        // Timing critical point #1: >= 17 ticks after clock edge until read of address/control lines
        r0 = REG_READ(GPIO_IN_REG);
        PROFILE1(XTHAL_GET_CCOUNT() - tscFall); 

        const int bankL1 = ((r0 & bankL1SelBits) >> bankL1SelShift);
        const int pageInBank = ((r0 & pageInBankSelBits) >> pageSelShift); 
        const uint32_t pageEn = banks[bankL1]->ctrl[pageInBank];
        uint8_t *pageData = banks[bankL1]->pages[pageInBank];
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, (pageEn | pinDrMask) & pinEnMask);

        uint16_t addr = r0 >> bus.addr.shift;
        ramAddr = &pageData[addr & pageOffsetMask];
        data = *ramAddr;
        REG_WRITE(GPIO_OUT1_REG, (data << bus.data.shift));
        // Timing critical point #2: Data output on bus before ~60 ticks
        PROFILE2(XTHAL_GET_CCOUNT() - tscFall);

        // keep the last address accessed in each page, use that to quickly implement cart bank switching 
        uint8_t page = ((r0 & bankL1SelBits) >> pageSelShift);
        uint8_t pageOffset = addr & 0xff;
        lastPageOffset[page] = pageOffset;
        banks[bankA0] = cartBanks[lastPageOffset[pageD5]]; // remap bank 0xa000 

        nextBmonHead = (bmonHead + 1) & bmonArraySzMask;               
        bmon = (r0 << bmonR0Shift);
        uint8_t *writeMux[2] = {ramAddr, &dummyWrite};
        AsmNops<4>::generate(); // boots at values 8-13 
        //while(XTHAL_GET_CCOUNT() - tscFall < 77) {}
        PROFILE3(XTHAL_GET_CCOUNT() - tscFall);
        uint32_t r1 = REG_READ(GPIO_IN1_REG);
        data = (r1 >> bus.data.shift);
        *writeMux[busWriteDisable] = data;

        // Timing critical point #4: All work done before ~120 ticks
        PROFILE4(XTHAL_GET_CCOUNT() - tscFall);     
    }
}
