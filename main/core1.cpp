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
//volatile DRAM_ATTR L1Block *testL1Blocks[pageSize] = {0};

template <CartBankSelect BankSelect>
__attribute__((section(".text.iloop_pbi"), noinline, used))
void iloop_pbi() {
    uint32_t bmon = 0, r0 = 0, r1 =0;
    unsigned int nextBmonHead = 1;
    uint8_t data = 0;
    uint8_t dummyWrite;
    uint8_t *ramAddr = &dummyWrite;

    while((dedic_gpio_cpu_ll_read_in()) != 0) {} // sync with clock before starting loop 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}

    // preparing to try and remove pinEnableMask, pinDriveMask and busWriteDisable globals and their
    // associated computations in core1 loop 
    // 
    // Step 1: see if we can use mmu map for controlling MPD signal.  MPD signal currently is controlled
    // with pinDriveMask 
    

    while(true) {    
        while((dedic_gpio_cpu_ll_read_in()) != 0) {} // wait for clock falling edge 
	PROFILE_LOOP_START();
	uint32_t tscFall = XTHAL_GET_CCOUNT();
	int bHead = bmonHead;
        bmonArray[bHead].r0 = r0;       
        bmonArray[bHead].r1 = r1;
        bmonHead = (bHead + 1) & bmonArraySzMask;
		// Update extended mem bank mapping based on d000Write data
                const int portb = d000Write[_0x301];
                const int extMemBank = (portb & 0x7c) >> 2;
                mmuState.l1Blocks[l1Block40] = mmuState.extBanks[extMemBank];
        AsmNops<LEADIN_NOPS>::generate(); 

        // Timing critical point #1: >= 17 ticks after clock edge until read of address/control lines
        r0 = REG_READ(GPIO_IN_REG);
        PROFILE1(XTHAL_GET_CCOUNT() - tscFall); 

        const int l1BlockIndex = ((r0 & bus.addr.mask) >> l1BlockSelShift);
        const int pageInL1Block = ((r0 & pageInL1BlockSelBits) >> pageSelShift)
              | ((r0 & bus.rw.mask) >> (bus.rw.shift - (pageBits - l1BlockBits)))   // can remove this statement if rw is moved
        ;  
        const uint32_t pageEn = mmuState.l1Blocks[l1BlockIndex]->ctrl[pageInL1Block];
        uint8_t *pageData = mmuState.l1Blocks[l1BlockIndex]->pages[pageInL1Block];
        
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, pageEn); //(pageEn | pinDrMask) & pinEnMask);
        PROFILE2(XTHAL_GET_CCOUNT() - tscFall);
        uint16_t addr = r0 >> bus.addr.shift;
        ramAddr = &pageData[addr & pageOffsetMask];

        const int isReadOp = ((r0 & bus.rw.mask) >> bus.rw.shift); // | busWriteDisable;
        if (__builtin_expect(isReadOp, 1)) { 
                data = *ramAddr;
                r1 = (data << bus.data.shift);
                REG_WRITE(GPIO_OUT1_REG, (data << bus.data.shift));
		PROFILE3(XTHAL_GET_CCOUNT() - tscFall);
                // Update BASIC ROM and PBI ROM memory mapping based on d000Write data
                mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
                int bankC0Select = (d000Write[_0x301] & 0x1) | ((d000Write[_0x1ff] & pbiDeviceNumMask));
                mmuState.l1Blocks[l1BlockC0] = mmuState.osEnL1BlockMux[bankC0Select];

                //AsmNops<25>::generate(); // about this much free time remains here 
                while(XTHAL_GET_CCOUNT() - tscFall < RD_EXIT_TS) {}
                REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
                PROFILE4(XTHAL_GET_CCOUNT() - tscFall);// 112-120 cycles seems to be the limits  // 
        } else {
                int page = pageNr(addr);
                lastPageOffset[page] = addr;

		// Update cartridge banking based on d500 write address or data, depending on scheme
                const int cartBankIndex =
                    (BankSelect == CartBankSelect::D500_ADDR_BANK_SEL)
                        ? (lastPageOffset[pageD5] & 0x1f)
                        : (d000Write[_0x500] & 0x1f);
                mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];
                mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
                AsmNops<0>::generate(); 
                 
                while(XTHAL_GET_CCOUNT() - tscFall < WR_EXIT_TS) {}
                r1 = REG_READ(GPIO_IN1_REG);
                REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
                PROFILE5(XTHAL_GET_CCOUNT() - tscFall);
                data = (r1 >> bus.data.shift);
                *ramAddr = data;
                PROFILE6(XTHAL_GET_CCOUNT() - tscFall);     
        }
    }
}

template void iloop_pbi<CartBankSelect::D500_ADDR_BANK_SEL>();
template void iloop_pbi<CartBankSelect::D500_DATA_BANK_SEL>();
