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
#include "const.h"
#include "bmon.h"
#include "util.h"
#include "mmu.h"
#include "cartridge.h"
#include "hwType.h"

//static constexpr DRAM_ATTR uint32_t l1BlockSelBits = (bus.rw.mask /*| bus.extDecode.mask*/ | bus.addr.mask); // R0 mask for page+addr
static constexpr DRAM_ATTR uint32_t pageInL1BlockSelBits = (bus.addr.mask & (l1BlockOffsetMask << bus.addr.shift)); // R0 mask for page index within a block
static constexpr DRAM_ATTR int l1BlockSelShift = (bus.extDecode.shift - l1BlockBits - 1); // R0 shift to get block number
static constexpr DRAM_ATTR int pageSelShift = (bus.extDecode.shift - pageBits - 1);     // R0 shift to get page number 

DRAM_ATTR uint8_t lastPageOffset[nrPages * (1 << PAGESEL_EXTRA_BITS)] = {0}; // offset within page of last mem access, for each page 

// One 6502 bus cycle, from the clock's falling edge: serve the cycle (read:
// drive the data bus; write: latch it) and follow the bank selects. r0
// carries the last cycle's GPIO_IN_REG, which bmon records.
// Always inlined into iloop_pbi(), so the production code is the same as
// the loop written out; csim can call it for one cycle at a time.
template <CartBankSelect BankSelect>
static inline __attribute__((always_inline)) void iloop_pbi_cycle(uint32_t &r0) {
    uint8_t data;
    uint8_t *ramAddr;

    PROFILE_LOOP_START();
    int bHead = bmonHead;
    bmonArray[bHead].r0 = r0;       
    bmonHead = (bHead + 1) & bmonArraySzMask;
    AsmNops<LEADIN_NOPS>::generate(); 

    // Timing critical point #1: >= 17 ticks after clock edge until read of address/control lines
    PROFILE1();
    r0 = REG_READ(GPIO_IN_REG);

    const int l1BlockIndex = ((r0 & bus.addr.mask) >> l1BlockSelShift);
    const int pageInL1Block = ((r0 & pageInL1BlockSelBits) >> pageSelShift)
          | ((r0 & bus.rw.mask) >> (bus.rw.shift - (pageBits - l1BlockBits)));
    // possibly remove this shift  ^^^ statement if RW pin is moved
      
    const uint32_t pageEn = mmuState.l1Blocks[l1BlockIndex]->ctrl[pageInL1Block];
    uint8_t *pageData = mmuState.l1Blocks[l1BlockIndex]->pages[pageInL1Block];
    
    PROFILE2();
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, pageEn); 
    uint16_t addr = r0 >> bus.addr.shift;
    ramAddr = &pageData[addr & pageOffsetMask];

    const int isReadOp = ((r0 & bus.rw.mask) >> bus.rw.shift);
    if (__builtin_expect(isReadOp, 1)) { 
        data = *ramAddr;
        PROFILE3();
        REG_WRITE(GPIO_OUT1_REG, (data << bus.data.shift));
        
        // Basic ROM select 
        mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
        
    // OS ROM/PBI ROM select
    int bankC0Select = (d000Write[_0x301] & 0x1) | ((d000Write[_0x1ff] & pbiDeviceNumMask));
        mmuState.l1Blocks[l1BlockC0] = mmuState.osEnL1BlockMux[bankC0Select];
        // Extended mem bank mapping based on D301
        const int portb = d000Write[_0x301];
        const int extMemBank = (portb & 0x7c) >> 2;
        mmuState.l1Blocks[l1Block40] = mmuState.extBanks[extMemBank];
        
        loopNezDelay(RD_EXIT_DELAY); 
        PROFILE4(); 
        REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
    } else {
        const int page = pageNr(addr);
        lastPageOffset[page] = addr;

        // Set cartridge bank index based on D500 write address or data, depending on scheme
        const int cartBankIndex =
            (BankSelect == CartBankSelect::D500_ADDR_BANK_SEL)
                ? (lastPageOffset[pageD5] & 0x1f)
                : (d000Write[_0x500] & 0x1f);
        mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];

        // Basic ROM select 
        mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
        
        loopNezDelay(WR_EXIT_DELAY); 
        PROFILE5();
        uint32_t r1 = REG_READ(GPIO_IN1_REG);
        PROFILE6();
        REG_WRITE(GPIO_ENABLE1_W1TC_REG, pinReleaseMask);
        data = (r1 >> bus.data.shift);
        *ramAddr = data;
    }
}

template <CartBankSelect BankSelect>
__attribute__((section(".text.iloop_pbi"), noinline, used))
void iloop_pbi() {
    uint32_t r0 = 0;

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
        iloop_pbi_cycle<BankSelect>(r0);
    }
}

template void iloop_pbi<CartBankSelect::D500_ADDR_BANK_SEL>();
template void iloop_pbi<CartBankSelect::D500_DATA_BANK_SEL>();

#ifdef CSIM
// csim: one 6502 bus cycle through iloop_pbi_cycle(), for the emulator's
// memory accesses (step 2 of running the MMU's map in csim). Sets up the
// GPIO inputs core 1 would see, runs the cycle, and reports what it drove.
// Returns true when the cycle drove the data bus (pageEn enabled the data
// pins: this board answers the read), with the byte in *data; on a write,
// *data is the byte written, and the cycle latches it where the MMU maps
// it, as on the hardware.
uint32_t GPIO_IN_REG, GPIO_IN1_REG, GPIO_OUT_REG, GPIO_OUT1_REG,
    GPIO_ENABLE_REG, GPIO_ENABLE1_REG, GPIO_ENABLE1_W1TS_REG, GPIO_ENABLE1_W1TC_REG;

bool csim_bus_cycle(uint16_t addr, bool read, uint8_t *data) {
    static uint32_t r0 = 0;
#ifdef HAVE_REFRESH
    static constexpr uint32_t notRefresh = bus.refresh_.mask;   // refresh_ is active low
#else
    static constexpr uint32_t notRefresh = 0;
#endif
    GPIO_IN_REG = (uint32_t(addr) << bus.addr.shift) | (read ? bus.rw.mask : 0) | notRefresh;
    GPIO_IN1_REG = read ? 0 : (uint32_t(*data) << bus.data.shift);
    GPIO_ENABLE1_W1TS_REG = GPIO_OUT1_REG = 0;

    if (atariCart.header.type == AtariCart::MegaCart128)
        iloop_pbi_cycle<CartBankSelect::D500_DATA_BANK_SEL>(r0);
    else
        iloop_pbi_cycle<CartBankSelect::D500_ADDR_BANK_SEL>(r0);

    const bool driven = read && (GPIO_ENABLE1_W1TS_REG & bus.data.mask) != 0;
    if (driven)
        *data = uint8_t(GPIO_OUT1_REG >> bus.data.shift);
    return driven;
}
#endif
