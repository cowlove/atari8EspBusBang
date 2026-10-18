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
    uint32_t bmon = 0, r0 = 0;
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
        //PROFILE_START();
        uint32_t tscFall = XTHAL_GET_CCOUNT();
        //nextBmonHead = (bHead + 1) & bmonArraySzMask;
        //bmonHead = nextBmonHead;
	int bHead = bmonHead;
        //bmon = (r0 << bmonR0Shift);
        //bmon = bmon | data;
        //bmonArray[bHead] = bmon;
        bmonArray[bHead] = r0;       
        bmonHead = (bHead + 1) & bmonArraySzMask;
        //uint32_t pinEnMask = pinEnableMask;
        //uint32_t pinDrMask = pinDriveMask;
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
        uint16_t addr = r0 >> bus.addr.shift;
        ramAddr = &pageData[addr & pageOffsetMask];

        const int isReadOp = ((r0 & bus.rw.mask) >> bus.rw.shift); // | busWriteDisable;
        if (__builtin_expect(isReadOp, 1)) { 
                data = *ramAddr;
                REG_WRITE(GPIO_OUT1_REG, (data << bus.data.shift));
                PROFILE2(XTHAL_GET_CCOUNT() - tscFall);

                // NOTE 4-way mux with bits combined for 0xd1ff and os enable is 0xd301 to select l1BlockC0
                // NOTE could pre-compute d000Write[_0x301] >> 1) & 0x1 and similar values 
                // NOTE if all mmu mapping is done here, could toss bmon (might still need it for bank psram swapping)  
                
                if constexpr (BankSelect == CartBankSelect::D500_DATA_BANK_SEL)
                    mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[1];
                else
                    mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
                //int bankC0Select = (((d000Write[_0x1ff] & pbiDeviceNumMask)));
                int bankC0Select = (d000Write[_0x301] & 0x1) | ((d000Write[_0x1ff] & pbiDeviceNumMask));
                mmuState.l1Blocks[l1BlockC0] = mmuState.osEnL1BlockMux[bankC0Select];
                //pinDrMask = (pinDrMask & bus.mpd.maskInverse) | ((d000Write[_0x1ff] & pbiDeviceNumMask) >> pbiDeviceNumShift << bus.mpd.shift); 

                const int portb = d000Write[_0x301];
                const int extMemBank = (portb & 0x7c) >> 2;
                mmuState.l1Blocks[l1Block40] = mmuState.extBanks[extMemBank];
                
                //AsmNops<25>::generate(); // about this much free time remains here 
                while(XTHAL_GET_CCOUNT() - tscFall < RD_EXIT_TS) {}
                REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
                PROFILE4(XTHAL_GET_CCOUNT() - tscFall);// 112-120 cycles seems to be the limits  // 
        } else {
                int page = pageNr(addr);
                lastPageOffset[page] = addr;
                if constexpr (BankSelect == CartBankSelect::D500_ADDR_BANK_SEL) {
                    const int cartBankIndex = lastPageOffset[pageD5] & 0x1f;
                    mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];
                    mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
                }
                AsmNops<0>::generate(); 
                 
                while(XTHAL_GET_CCOUNT() - tscFall < WR_EXIT_TS) {}
                uint32_t r1 = REG_READ(GPIO_IN1_REG);
                PROFILE3(XTHAL_GET_CCOUNT() - tscFall);
                REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
                data = (r1 >> bus.data.shift);
                //uint8_t *writeMux = {ramAddr, &dummyWrite);}
                //*writeMux[busWriteDisable] = data;
                //AsmNops<5>::generate(); // about this much free time remains here 
                //while(XTHAL_GET_CCOUNT() - tscFall < 95) {}  // PROF5 == 113 w/o delay loop, min 115 w delay loop
                *ramAddr = data;
                if constexpr (BankSelect == CartBankSelect::D500_DATA_BANK_SEL) {
                    if (addr == _0xd500) {
                        const uint8_t bankSelect = d000Write[_0x500];
                        const int requestedBank = bankSelect & 0x7f;
                        const int cartBankIndex = (bankSelect & 0x80) || requestedBank >= 32
                            ? 0x1f
                            : requestedBank;
                        mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];
                        mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[1];
                    }
                }
                PROFILE5(XTHAL_GET_CCOUNT() - tscFall);     
        }
    }
}

template void iloop_pbi<CartBankSelect::D500_ADDR_BANK_SEL>();
template void iloop_pbi<CartBankSelect::D500_DATA_BANK_SEL>();
