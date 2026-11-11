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

// Cycle timing (headNops, rdDataNops, rdReleaseDelay, wrSampleNops,
// wrReleaseDelay) comes straight from hwType.h's per-machine table, each
// an exact cycle count for loopNezDelay() (util.h).

DRAM_ATTR uint8_t lastPageOffset[nrPages * (1 << PAGESEL_EXTRA_BITS)] = {0}; // offset within page of last mem access, for each page 

#ifdef CSIM
// ===========================================================================
// csim: the reference version of one bus cycle, in plain C++.
//
// This is what the ESP32 loop below (iloop_pbi()) is meant
// to do, written for reading: no timing, no asm, no register or scheduling
// tricks, and the cartridge bank-select scheme picked at run time instead of
// by template. csim runs it for each 6502 access atari800 makes (BUS_CYCLE=1),
// so the emulated Atari sees the board's memory map. The two must agree: a
// change to what the ESP cycle does (not just how fast) belongs here too.
//
// Returns whether the board asserted extSel_, i.e. handled the access: on a
// read, *data is the byte it drove; a write is latched where the MMU maps it.
// Otherwise the access is the Atari's own (its RAM, ROMs, I/O chips), though
// the board has still seen it, as on the hardware: a write still lands in the
// page the MMU maps for it (e.g. the d000Write shadow of the chip registers).
//
// Not modeled (see csim/a800/README.md): halt_ and PSRAM swaps, mpd_,
// ANTIC's DMA, and all bus timing.
//
// GPIO registers other csim code still writes (REG_WRITE() in main.cpp),
// as plain variables (csim.h).
uint32_t GPIO_IN_REG, GPIO_IN1_REG, GPIO_OUT_REG, GPIO_OUT1_REG,
    GPIO_ENABLE_REG, GPIO_ENABLE1_REG, GPIO_ENABLE1_W1TS_REG, GPIO_ENABLE1_W1TC_REG;

bool csim_bus_cycle(uint16_t addr, bool read, uint8_t *data) {
    // Head (ESP: before P1). Map the ext bank ($4000) and the OS/PBI ROM
    // ($C000) from the PORTB and $D1FF values the last write cycles left,
    // so a switch written last cycle applies to this one, as on the real
    // MMU. The ESP loads these before it knows the address; they override
    // mmuState.l1Blocks[] for those two blocks, which isn't kept current.
    const uint8_t portb = d000Write[_0x301];
    const uint8_t d1ff = d000Write[_0x1ff];
    L1Block *const block40 = mmuState.extBanks[portb];
    L1Block *const blockC0 = mmuState.osEnL1BlockMux[(portb & 0x1) | (d1ff & pbiDeviceNumMask)];

    // P1: the address and R/W lines. Decode them to the 16 KB block and the
    // page's entry in it; each page has separate read and write entries.
    const int page = pageNr(addr);
    const int l1BlockIndex = page2L1Block(page);
    const int pageInL1Block = (page & pageInL1BlockMask) | (read ? PAGESEL_RD : PAGESEL_WR) | PAGESEL_CPU;
    L1Block *block = mmuState.l1Blocks[l1BlockIndex];
    if (l1BlockIndex == l1Block40) block = block40;
    if (l1BlockIndex == l1BlockC0) block = blockC0;

    // P2: the page's bus-control word: which pins the board drives this
    // cycle (extSel_ to take the access from the Atari, the data bus on a
    // read, sometimes halt_ or mpd_). The ESP writes it to the pin enables.
    const uint32_t pageEn = block->ctrl[pageInL1Block];
    const bool extSelAsserted = (pageEn & bus.extSel.mask) != 0;
    uint8_t *const ramAddr = &block->pages[pageInL1Block][addr & pageOffsetMask];

    if (read) {
        // P3: drive the byte (the ESP always loads it; the data pins only
        // reach the bus if pageEn enabled them).
        const uint8_t value = *ramAddr;
        if (extSelAsserted)
            *data = value;
        // BASIC/cart at $A000-$BFFF follows PORTB bit 1, one access late.
        mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
        // P4: release the bus.
    } else {
        // Remember each page's last write offset: the D500_ADDR cartridge
        // scheme selects the bank by the low address bits of a $D5xx write.
        lastPageOffset[page] = addr;

        // Cartridge bank select, from the $D5xx write's address (most
        // schemes) or, for MegaCart, $D500's data. MegaCart reads it before
        // this cycle stores the new data, so its switch shows a write later.
        const int cartBankIndex = (atariCart.header.type == AtariCart::MegaCart128)
                                      ? (d000Write[_0x500] & 0x1f)
                                      : (lastPageOffset[pageD5] & 0x1f);
        mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];
        // Map it now, so the cart switch reaches the next cycle.
        mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];

        // Record the write for core 0 (bmonArray, as the address/control
        // GPIO word core 1 read at P1), then P5: latch the data and store it
        // where the MMU maps the page. Only then publish the entry: core 0
        // acts on the written value (d000Write[], a PBI request) as soon as
        // it sees it. P6: release the bus.
#ifdef HAVE_REFRESH
        static constexpr uint32_t notRefresh = bus.refresh_.mask;   // refresh_ is active low
#else
        static constexpr uint32_t notRefresh = 0;
#endif
        const unsigned int bHead = bmonHead;
        bmonArray[bHead].r0 = (uint32_t(addr) << bus.addr.shift) | notRefresh;   // R/W low: a write
        *ramAddr = *data;
        bmonHead = (bHead + 1) & bmonArraySzMask;
    }
    return extSelAsserted;
}

#else  // ESP32 core 1 from here on

// iloop_pbi(): each pass is one 6502 bus cycle, from the clock's falling
// edge: map the banks the last write selected, then serve the cycle (read:
// drive the data bus; write: latch it). r0 gets this cycle's GPIO_IN_REG.
// csim_bus_cycle() above is the same cycle in plain C++.
//
// bmonPush(): record a write cycle's GPIO_IN_REG (address and control lines) for core 0,
// in the write path's idle time before the data sample. Reads aren't
// recorded: core 0 only acts on writes.
// bmonPush() writes the entry; bmonPublish() makes it visible to core 0,
// after the cycle's data store, since core 0 reads what was written
// (d000Write[], a PBI request) as soon as it sees the entry.
static inline __attribute__((always_inline)) int bmonPush(uint32_t r0) {
    int bHead = bmonHead;
    bmonArray[bHead].r0 = r0;
    int next = (bHead + 1) & bmonArraySzMask;
    __asm__ volatile ("" : "+r" (next));   // compute it here, not in the tail
    return next;
}
static inline __attribute__((always_inline)) void bmonPublish(int next) {
    __asm__ volatile ("" ::: "memory");   // after *ramAddr = data
    bmonHead = next;
}

// RELEASE_PLACEMENT (experiment, -D only): where the bus pins are released.
//   0: end of each path, after its release delay (production)
//   1: jump, poll, release, head ... P1 (release right after the clock fall)
//   2: jump, poll, head ... P1, release (release right after the address read)
//   3: poll, release, jump, head ... P1 (each path's tail waits for the
//      clock fall and releases; the jump back fills release-to-P1 time)
// 1-3 don't use rdReleaseDelay/wrReleaseDelay.
#ifndef RELEASE_PLACEMENT
#define RELEASE_PLACEMENT 0
#endif
// mmuState.pinReleaseMask changes at run time (core 0 toggles the halt
// and IRQ bits), so it's loaded every cycle, at offset 0 from &mmuState.
static inline __attribute__((always_inline)) void releasePins() {
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, mmuState.pinReleaseMask);
}
// A distinct, codeless asm per tail, so GCC can't merge the two tails.
template <int Tag> static inline __attribute__((always_inline)) void tailBarrier() {
    __asm__ volatile (";;; # tail %0" :: "i" (Tag));
}
// Wait for the clock's falling edge.
static inline __attribute__((always_inline)) void waitClockFall() {
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
}

template <CartBankSelect BankSelect>
__attribute__((section(".text.iloop_pbi"), noinline, used))
void iloop_pbi() {
    while((dedic_gpio_cpu_ll_read_in()) != 0) {} // sync with clock before starting loop 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}

#if RELEASE_PLACEMENT == 3
    waitClockFall();
#endif
    while(true) {
#if RELEASE_PLACEMENT != 3
        PROFILE_GPIO_PIN_CLEAR();
        waitClockFall();
#endif
        // One 6502 bus cycle, from the clock's falling edge.
        uint8_t data;
        uint8_t *ramAddr;
    #if RELEASE_PLACEMENT == 1
        PROFILE4();
        PROFILE6();
        releasePins();
    #endif

        PROFILE_LOOP_START();

        // Map the ext bank and OS/PBI select (PORTB, $D1FF) before this cycle's
        // address read, so a switch written last cycle applies to this one, as
        // on the real MMU. The OS select must be on time: the OS-to-RAM copy
        // trick re-enables the ROM from code in $C000-$FFFF, and a late board
        // would drive RAM over it. BASIC ($A000) is left to the exit paths to
        // keep the head short; it's one read late, as before.
        // Both reads share one base register. With registers short (the moveqz
        // decode), GCC otherwise reloads &d000Write[0x100] for $D1FF from the
        // IRAM literal pool with an l32r every cycle, before the address read.
        const uint8_t *d000Hi = &d000Write[0x300];
        __asm__ ("" : "+r" (d000Hi));
        const uint32_t portb = d000Hi[_0x301 - 0x300];
        uint32_t d1ff = d000Hi[_0x1ff - 0x300];
        __asm__ ("" : "+r" (d1ff));   // else GCC ORs in bytes and zero-extends again
        const uint32_t osSel = (portb & 0x1) | (d1ff & pbiDeviceNumMask);
        // Load the remapped $4000 and $C000 blocks into registers; the decode
        // below picks them by index. Storing them into mmuState.l1Blocks[] here
        // and loading back from it after the address read stalled ~5 cycles
        // between the address read (P1) and the pin enable (P2) on the bench.
        // mmuState.l1Blocks[l1Block40] and [l1BlockC0] aren't kept current:
        // core 0 code reads those two through mmuBlock() (mmu.h).
        L1Block *const block40 = mmuState.extBanks[portb];
        L1Block *const blockC0 = mmuState.osEnL1BlockMux[osSel];
        loopNezDelay(headNops); 

        // Timing critical point #1: >= 17 ticks after clock edge until read of address/control lines
        PROFILE1();
        uint32_t r0 = REG_READ(GPIO_IN_REG);
    #if RELEASE_PLACEMENT == 2
        PROFILE4();
        PROFILE6();
        releasePins();
        __asm__ volatile ("" ::: "memory");   // keep the decode loads after it
    #endif

        const int l1BlockIndex = ((r0 & bus.addr.mask) >> l1BlockSelShift);
        const int pageInL1Block = ((r0 & pageInL1BlockSelBits) >> pageSelShift)
              | ((r0 & bus.rw.mask) >> (bus.rw.shift - (pageBits - l1BlockBits)));
        // possibly remove this shift  ^^^ statement if RW pin is moved
      
        L1Block *block = mmuState.l1Blocks[l1BlockIndex];
        // Branchless (GCC emits beqi branches for the ternaries).
        const int d40 = l1BlockIndex - l1Block40, dC0 = l1BlockIndex - l1BlockC0;
        __asm__ ("moveqz %0, %1, %2" : "+r" (block) : "r" (block40), "r" (d40));
        __asm__ ("moveqz %0, %1, %2" : "+r" (block) : "r" (blockC0), "r" (dC0));
        const uint32_t pageEn = block->ctrl[pageInL1Block];

        // Timing critical point #2: extSel must be asserted before phi2 rises,
        // or the Atari's own RAM/ROM starts its cycle and fights the bus (a
        // 6-cycle-later P1/P2 fails to boot; a 6-cycle-later data drive is fine).
        // So only the pin-enable lookup comes before the store: the barrier,
        // which also renews r0, keeps the data-pointer load and the address
        // extract after it, where GCC had scheduled them ahead of the store.
        PROFILE2();
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, pageEn);
        __asm__ volatile ("" : "+r" (r0) :: "memory");
        uint8_t *pageData = block->pages[pageInL1Block];
        uint16_t addr = r0 >> bus.addr.shift;
        ramAddr = &pageData[addr & pageOffsetMask];

        const int isReadOp = ((r0 & bus.rw.mask) >> bus.rw.shift);
        if (__builtin_expect(isReadOp, 1)) { 
            data = *ramAddr;
            loopNezDelay(rdDataNops);
            PROFILE3();
            REG_WRITE(GPIO_OUT1_REG, (data << bus.data.shift));
            mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
    #if RELEASE_PLACEMENT == 0
            loopNezDelay(rdReleaseDelay);
            PROFILE4(); 
            REG_WRITE(GPIO_ENABLE1_W1TC_REG, mmuState.pinReleaseMask);
    #elif RELEASE_PLACEMENT == 3
            PROFILE_GPIO_PIN_CLEAR();
            waitClockFall();
            PROFILE4();
            releasePins();
            tailBarrier<1>();
    #endif
        } else {
            const int page = pageNr(addr);
            lastPageOffset[page] = addr;

            // Set cartridge bank index based on D500 write address or data, depending on scheme
            const int cartBankIndex =
                (BankSelect == CartBankSelect::D500_ADDR_BANK_SEL)
                    ? (lastPageOffset[pageD5] & 0x1f)
                    : (d000Write[_0x500] & 0x1f);
            mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];
            // Map it now, so the cart switch reaches the next cycle.
            mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];
            const int bmonNext = bmonPush(r0);
            loopNezDelay(wrSampleNops);
            PROFILE5();
            uint32_t r1 = REG_READ(GPIO_IN1_REG);
    #if RELEASE_PLACEMENT == 0
            loopNezDelay(wrReleaseDelay);
            PROFILE6();
            REG_WRITE(GPIO_ENABLE1_W1TC_REG, mmuState.pinReleaseMask);
    #endif
            data = (r1 >> bus.data.shift);
            *ramAddr = data;
            bmonPublish(bmonNext);
    #if RELEASE_PLACEMENT == 3
            PROFILE_GPIO_PIN_CLEAR();
            waitClockFall();
            PROFILE6();
            releasePins();
            tailBarrier<2>();
    #endif
        }
    }
}

template void iloop_pbi<CartBankSelect::D500_ADDR_BANK_SEL>();
template void iloop_pbi<CartBankSelect::D500_DATA_BANK_SEL>();

#endif  // CSIM
