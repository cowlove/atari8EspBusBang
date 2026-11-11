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
    const bool extSelAsserted = (pageEn & ctrlExtSel) != 0;
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
static_assert(!(EXTSEL_DEDIC == 1 && RELEASE_PLACEMENT != 0),
              "EXTSEL_DEDIC=1 releases in the RELEASE_PLACEMENT=0 tails; use EXTSEL_DEDIC=2");
// mmuState.pinReleaseMask changes at run time (core 0 toggles the halt
// and IRQ bits), so it's loaded every cycle, at offset 0 from &mmuState.
// gpioEnableClrReg: GPIO_ENABLE1_W1TC_REG, held in a register (iloop_pbi()).
static inline __attribute__((always_inline)) void releasePins(uint32_t gpioEnableClrReg) {
    REG_WRITE(gpioEnableClrReg, mmuState.pinReleaseMask);
}
// EXTSEL_DEDIC (pinDefs.h): extSel_ and mpd_ on core 1's dedicated
// outputs. Assert: the page's ctrl word carries them at their channels'
// bit positions (ctrlExtSel, ctrlMpd), so writing it as both value and
// mask sets exactly the channels this page asserts, in one instruction and
// no extra register; the rest are left alone. Release clears both, by an
// immediate mask.
static inline __attribute__((always_inline)) void extSelAssert(uint32_t pageEn) {
#if EXTSEL_DEDIC
    __asm__ volatile ("ee.wr_mask_gpio_out %0, %0" :: "r" (pageEn));
#endif
    (void)pageEn;
}
static inline __attribute__((always_inline)) void extSelRelease() {
#if EXTSEL_DEDIC
    __asm__ volatile ("ee.clr_bit_gpio_out %0" :: "i" (extSelDedicMask | mpdDedicMask));
#endif
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

// The clock poll, in asm so GCC can't lay it out again (RELEASE_PLACEMENT
// 0-2). Each path's tail ends with one probe of its own: if the clock has
// already fallen (a late cycle), a taken branch goes straight to the next
// cycle's head, 4 cycles from the end of the tail on either path. Otherwise
// it jumps to the shared wait (ILOOP_WAIT_FALL), whose unrolled probes take
// 2 cycles each, the same resolution as before. The tag keeps the two
// tails' asm distinct, so GCC can't merge them into one shared tail.
// Macros, not functions: asm goto can only reach labels in its own function.
#define ILOOP_TAIL_POLL(tag) do {                                          \
        PROFILE_GPIO_PIN_CLEAR();                                          \
        uint32_t clk_;                                                     \
        __asm__ goto ("ee.get_gpio_in %0   # tail poll " #tag "\n\t"       \
                      "beqz %0, %l[cycleTop]\n\t"                          \
                      "j %l[waitFall]"                                     \
                      : "=r" (clk_) ::: cycleTop, waitFall);               \
        __builtin_unreachable();                                           \
    } while (0)
// The read tail with no release delay (RELEASE_PLACEMENT 1-2) can finish
// while this cycle's phi1 is still low, and the level-triggered poll would
// then start a bogus cycle on the same clock (seen on the bench: a second
// loop end one pass later). So it first waits for the clock to go high,
// then for the fall. Only the read tail: it ends ~50 ticks before the next
// fall, so it can't be arriving late; a late tail that did this would wait
// out the whole next cycle and miss it.
#define ILOOP_TAIL_RISE_THEN_WAIT(tag) do {                                \
        PROFILE_GPIO_PIN_CLEAR();                                          \
        uint32_t clk_;                                                     \
        __asm__ goto ("1:\n\t"                                             \
                      "ee.get_gpio_in %0   # tail rise wait " #tag "\n\t"  \
                      "beqz %0, 1b\n\t"                                    \
                      "j %l[waitFall]"                                     \
                      : "=r" (clk_) ::: waitFall);                         \
        __builtin_unreachable();                                           \
    } while (0)
#define ILOOP_WAIT_FALL() do {                                             \
        uint32_t clk_;                                                     \
        __asm__ goto ("1:\n\t"                                             \
                      ".rept 8\n\t"                                        \
                      "ee.get_gpio_in %0\n\t"                              \
                      "beqz %0, %l[cycleTop]\n\t"                          \
                      ".endr\n\t"                                          \
                      "j 1b"                                               \
                      : "=r" (clk_) ::: cycleTop);                         \
        __builtin_unreachable();                                           \
    } while (0)

template <CartBankSelect BankSelect>
__attribute__((section(".text.iloop_pbi"), noinline, used))
void iloop_pbi() {
    while((dedic_gpio_cpu_ll_read_in()) != 0) {} // sync with clock before starting loop 
    while((dedic_gpio_cpu_ll_read_in()) == 0) {}

    // The address-read (P1), pin-enable (P2) and release register
    // addresses, made opaque so GCC keeps them in registers across the
    // loop. With the asm goto poll it otherwise reloads them from the IRAM
    // literal pool every cycle: before P1 (core1_cycle_count.py --strict),
    // or before each tail's release (an l32r per tail).
    uint32_t gpioInReg = GPIO_IN_REG, gpioEnableSetReg = GPIO_ENABLE1_W1TS_REG;
    uint32_t gpioEnableClrReg = GPIO_ENABLE1_W1TC_REG;
    __asm__ ("" : "+r" (gpioInReg), "+r" (gpioEnableSetReg), "+r" (gpioEnableClrReg));
    // Likewise PORTB's and $D1FF's shadow bytes, which the head reads every
    // cycle: one register each, and no per-cycle address arithmetic ($D1FF
    // is out of l8ui's 0-255 offset range from PORTB's). Not volatile: GCC
    // then zero-extends each loaded byte again (2 head cycles).
    const uint8_t *portbShadow = &d000Write[_0x301];
    const uint8_t *d1ffShadow = &d000Write[_0x1ff];
    __asm__ ("" : "+r" (portbShadow), "+r" (d1ffShadow));
#if RELEASE_PLACEMENT == 3
    waitClockFall();
    goto cycleTop;
#else
    goto waitFall;
#endif
cycleTop:
    {
        // One 6502 bus cycle, from the clock's falling edge.
        uint8_t data;
        uint8_t *ramAddr;
    #if EXTSEL_DEDIC == 2
        extSelRelease();   // last cycle's: one instruction, no APB access
    #endif
    #if RELEASE_PLACEMENT == 1
        PROFILE4();
        PROFILE6();
        releasePins(gpioEnableClrReg);
    #endif

        PROFILE_LOOP_START();

        // Map the ext bank and OS/PBI select (PORTB, $D1FF) before this cycle's
        // address read, so a switch written last cycle applies to this one, as
        // on the real MMU. The OS select must be on time: the OS-to-RAM copy
        // trick re-enables the ROM from code in $C000-$FFFF, and a late board
        // would drive RAM over it. BASIC ($A000) is left to the exit paths to
        // keep the head short; it's one read late, as before.
        // Both addresses are registers set up before the loop (above): with
        // registers short, GCC otherwise reloads them from the IRAM literal
        // pool with an l32r every cycle, before the address read (546f756).
        const uint32_t portb = *portbShadow;
        uint32_t d1ff = *d1ffShadow;
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
        uint32_t r0 = REG_READ(gpioInReg);
    #if RELEASE_PLACEMENT == 2
        PROFILE4();
        PROFILE6();
        releasePins(gpioEnableClrReg);
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
        extSelAssert(pageEn);
        REG_WRITE(gpioEnableSetReg, pageEn);
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
            mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(*portbShadow >> 1) & 0x1];
    #if RELEASE_PLACEMENT == 0
            loopNezDelay(rdReleaseDelay);
            PROFILE4(); 
            REG_WRITE(gpioEnableClrReg, mmuState.pinReleaseMask);
    #if EXTSEL_DEDIC == 1
            extSelRelease();
    #endif
    #elif RELEASE_PLACEMENT == 3
            PROFILE_GPIO_PIN_CLEAR();
            waitClockFall();
            PROFILE4();
            releasePins(gpioEnableClrReg);
            tailBarrier<1>();
            goto cycleTop;
    #endif
    #if RELEASE_PLACEMENT == 0
            ILOOP_TAIL_POLL(read);
    #elif RELEASE_PLACEMENT != 3
            ILOOP_TAIL_RISE_THEN_WAIT(read);
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
            mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(*portbShadow >> 1) & 0x1];
            const int bmonNext = bmonPush(r0);
            loopNezDelay(wrSampleNops);
            PROFILE5();
            uint32_t r1 = REG_READ(GPIO_IN1_REG);
    #if RELEASE_PLACEMENT == 0
            loopNezDelay(wrReleaseDelay);
            PROFILE6();
            REG_WRITE(gpioEnableClrReg, mmuState.pinReleaseMask);
    #if EXTSEL_DEDIC == 1
            extSelRelease();
    #endif
    #endif
            data = (r1 >> bus.data.shift);
            *ramAddr = data;
            bmonPublish(bmonNext);
    #if RELEASE_PLACEMENT == 3
            PROFILE_GPIO_PIN_CLEAR();
            waitClockFall();
            PROFILE6();
            releasePins(gpioEnableClrReg);
            tailBarrier<2>();
            goto cycleTop;
    #else
            ILOOP_TAIL_POLL(write);
    #endif
        }
    }
#if RELEASE_PLACEMENT != 3
waitFall:
    ILOOP_WAIT_FALL();
#endif
}

template void iloop_pbi<CartBankSelect::D500_ADDR_BANK_SEL>();
template void iloop_pbi<CartBankSelect::D500_DATA_BANK_SEL>();

#endif  // CSIM
