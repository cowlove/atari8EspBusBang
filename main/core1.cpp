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

// Cycle timing (headNops, rdDataNops, wrSampleNops) comes straight from
// hwType.h's per-machine table, each an exact cycle count for
// loopNezDelay() (util.h).

DRAM_ATTR uint8_t lastPageOffset[nrPages * (1 << PAGESEL_EXTRA_BITS)] = {0}; // offset within page of last mem access, for each page

// How core 1 drives the bus, per 6502 cycle (both versions below):
//   - extSel_ and mpd_ come from core 1's dedicated GPIO outputs (pinDefs.h),
//     so they change within a few ticks of the instruction. Both are sampled
//     at phi2's rising edge (30-50 ns setup), so they are asserted as early
//     as the decode allows, and released right after the clock's fall.
//   - The data bus (and halt_/irq_, which core 0 sets in pinReleaseMask) go
//     through the GPIO_ENABLE1 registers, an APB store whose pin edge lands
//     ~25 ticks later. They are enabled with extSel_ and released after the
//     address read (P1), so that store doesn't stall the read.
//   - Each path ends by waiting for the next fall. The read path finishes
//     early, possibly while this cycle's phi1 is still low, so it first
//     waits for the clock to go high.

#ifdef CSIM
// ===========================================================================
// csim: the reference version of iloop_pbi() below, one bus cycle per call.
//
// This is what the ESP32 loop is meant to do, written for reading: the same
// steps in the same order, with the same register reads and writes, but in
// plain C++. No timing delays, no asm, no register-pinning or scheduling
// tricks, and the cartridge bank-select scheme picked at run time instead of
// by template. When the ESP32 loop changes what it does (not just how fast),
// change this too.
//
// csim runs it for each 6502 access atari800 makes (BUS_CYCLE=1), so the
// emulated Atari sees the board's memory map. The GPIO registers are plain
// variables (csim.h); the call sets the inputs core 1 would see for this
// access, runs the cycle, and reads back what it drove. The clock is fake:
// dedic_gpio_cpu_ll_read_in() toggles on every read (csim.h), so each wait
// for an edge ends within two reads.
//
// Returns whether the board asserted extSel_, i.e. handled the access: on a
// read, *data is the byte it drove; a write is latched where the MMU maps it.
// Otherwise the access is the Atari's own (its RAM, ROMs, I/O chips), though
// the board has still seen it, as on the hardware: a write still lands in the
// page the MMU maps for it (e.g. the d000Write shadow of the chip registers).
//
// Not modeled (see csim/a800/README.md): halt_ and PSRAM swaps, mpd_'s
// effect, ANTIC's DMA, and all bus timing.
uint32_t GPIO_IN_REG, GPIO_IN1_REG, GPIO_OUT_REG, GPIO_OUT1_REG,
    GPIO_ENABLE_REG, GPIO_ENABLE1_REG, GPIO_ENABLE1_W1TS_REG, GPIO_ENABLE1_W1TC_REG;

bool csim_bus_cycle(uint16_t busAddr, bool busRead, uint8_t *data) {
    // The bus for this access: the address and R/W lines (read at P1) and,
    // on a write, the data lines (sampled at P5).
#ifdef HAVE_REFRESH
    static constexpr uint32_t notRefresh = bus.refresh_.mask;   // refresh_ is active low
#else
    static constexpr uint32_t notRefresh = 0;
#endif
    GPIO_IN_REG = (uint32_t(busAddr) << bus.addr.shift) | (busRead ? bus.rw.mask : 0) | notRefresh;
    GPIO_IN1_REG = busRead ? 0 : (uint32_t(*data) << bus.data.shift);

    // ---- one pass of iloop_pbi(), starting at the clock's falling edge ----

    // Release last cycle's extSel_ and mpd_.
    dedic_gpio_cpu_ll_write_mask(bus.ctrlExtSel.mask | bus.ctrlMpd.mask, 0);

    // Head: pick the ext bank ($4000) and the OS/PBI ROM ($C000) from the
    // PORTB and $D1FF values the last write cycles left, so a switch written
    // last cycle applies to this one, as on the real MMU.
    const uint8_t portb = d000Write[_0x301];
    const uint8_t d1ff = d000Write[_0x1ff];
    L1Block *const block40 = mmuState.extBanks[portb];
    L1Block *const blockC0 = mmuState.osEnL1BlockMux[(portb & 0x1) | (d1ff & pbiDeviceNumMask)];

    // P1: read the address and R/W lines.
    const uint32_t r0 = REG_READ(GPIO_IN_REG);

    // Map those two blocks for this cycle's decode (the ESP32 loop does it
    // while the address read is still in flight).
    mmuState.l1Blocks[l1Block40] = block40;
    mmuState.l1Blocks[l1BlockC0] = blockC0;

    // Release last cycle's data bus (and halt_/irq_ when core 0 has lifted
    // them): after the fall, and after P1 so the store doesn't delay it.
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, mmuState.pinReleaseMask);

    // Decode: the 16 KB block, and the page's entry in it (each page has
    // separate read and write entries).
    const uint16_t addr = r0 >> bus.addr.shift;
    const bool read = (r0 & bus.rw.mask) != 0;
    const int page = pageNr(addr);
    const int l1BlockIndex = page2L1Block(page);
    const int pageInL1Block = (page & pageInL1BlockMask) | (read ? PAGESEL_RD : PAGESEL_WR) | PAGESEL_CPU;
    L1Block *const block = mmuState.l1Blocks[l1BlockIndex];

    // P2: the page's bus-control word says which lines the board drives this
    // cycle: extSel_ to take the access from the Atari, mpd_ for the PBI ROM
    // at $D800, the data bus on a read, sometimes halt_. extSel_ and mpd_
    // first: their ctrl bits sit at their dedicated channels' positions
    // (pinDefs.h), so writing the word as both mask and value sets exactly
    // the ones this page asserts. Then the pin enables.
    const uint32_t pageEn = block->ctrl[pageInL1Block];
    dedic_gpio_cpu_ll_write_mask(pageEn, pageEn);
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, pageEn);
    uint8_t *const ramAddr = &block->pages[pageInL1Block][addr & pageOffsetMask];

    if (read) {
        // P3: drive the byte (it reaches the bus only if pageEn enabled the
        // data pins).
        REG_WRITE(GPIO_OUT1_REG, uint32_t(*ramAddr) << bus.data.shift);
        // MegaCart: select the bank from $D500's data here too, so a bank
        // write reaches the mapping after the next cycle, read or write (the
        // write path picks it up only on the next write, below).
        if (atariCart.header.type == AtariCart::MegaCart128)
            mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[d000Write[_0x500] & 0x1f];
        // BASIC/cart at $A000-$BFFF follows PORTB bit 1, one access late.
        mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];

        // Tail: wait out phi1 if it is still low, then wait for the fall.
        while (dedic_gpio_cpu_ll_read_in() == 0) {}
        while (dedic_gpio_cpu_ll_read_in() != 0) {}
    } else {
        // Remember each page's last write offset: the D500_ADDR cartridge
        // scheme selects the bank by the low address bits of a $D5xx write.
        lastPageOffset[page] = addr;

        // Cartridge bank select, from the $D5xx write's address (most
        // schemes) or, for MegaCart, $D500's data. MegaCart reads it before
        // this cycle stores the new data, so its switch takes effect after
        // the next cycle (the read path above picks it up too): one cycle
        // late. Mapping after the data sample would make it exact, at the
        // cost of the write path, the loop's longest.
        const int cartBankIndex = (atariCart.header.type == AtariCart::MegaCart128)
                                      ? (d000Write[_0x500] & 0x1f)
                                      : (lastPageOffset[pageD5] & 0x1f);
        mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];
        // Map it now, so the cart switch reaches the next cycle.
        mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];

        // Record the write for core 0 (the address/control word from P1).
        const unsigned int bHead = bmonHead;
        bmonArray[bHead].r0 = r0;

        // P5: sample the data lines and store the byte where the MMU maps
        // the page. Only then publish the bmon entry: core 0 acts on the
        // written value (d000Write[], a PBI request) as soon as it sees it.
        const uint32_t r1 = REG_READ(GPIO_IN1_REG);
        *ramAddr = r1 >> bus.data.shift;
        bmonHead = (bHead + 1) & bmonArraySzMask;

        // Tail: wait for the fall.
        while (dedic_gpio_cpu_ll_read_in() != 0) {}
    }

    // For csim's caller: did this board take the access, and what did it
    // drive?
    const bool extSelAsserted = (dedic_gpio_cpu_ll_read_out() & bus.ctrlExtSel.mask) != 0;
    if (extSelAsserted && read)
        *data = uint8_t(REG_READ(GPIO_OUT1_REG) >> bus.data.shift);
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

// extSel_ and mpd_ on core 1's dedicated outputs (pinDefs.h). Assert: the
// page's ctrl word carries them at their channels' bit positions, so
// writing it as both value and mask sets exactly the channels this page
// asserts, in one instruction and no extra register; the rest are left
// alone. Release clears both, by an immediate mask.
static inline __attribute__((always_inline)) void extSelAssert(uint32_t pageEn) {
    __asm__ volatile ("ee.wr_mask_gpio_out %0, %0" :: "r" (pageEn));
}
static inline __attribute__((always_inline)) void extSelRelease() {
    __asm__ volatile ("ee.clr_bit_gpio_out %0" :: "i" (bus.ctrlExtSel.mask | bus.ctrlMpd.mask));
}

// The clock poll, in asm so GCC can't lay it out again. The write tail ends
// with one probe of its own: if the clock has already fallen (a late cycle),
// a taken branch goes straight to the next cycle's head, 4 cycles from the
// end of the tail. Otherwise it jumps to the shared wait (ILOOP_WAIT_FALL),
// whose unrolled probes take 2 cycles each. The tag keeps the tails' asm
// distinct, so GCC can't merge them into one shared tail.
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
// The read tail can finish while this cycle's phi1 is still low, and the
// level-triggered poll would then start a bogus cycle on the same clock
// (seen on the bench: a second loop end one pass later). So it first waits
// for the clock to go high, then for the fall. Only the read tail: it ends
// ~50 ticks before the next fall, so it can't be arriving late; a late tail
// that did this would wait out the whole next cycle and miss it.
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
    // or before the release (an l32r per cycle).
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
    goto waitFall;
cycleTop:
    {
        // One 6502 bus cycle, from the clock's falling edge.
        uint8_t data;
        uint8_t *ramAddr;
        extSelRelease();   // last cycle's extSel_/mpd_: one instruction, no APB access

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
        // Load the remapped $4000 and $C000 blocks into registers; they go
        // into mmuState.l1Blocks[] right after the address read (below).
        L1Block *const block40 = mmuState.extBanks[portb];
        L1Block *const blockC0 = mmuState.osEnL1BlockMux[osSel];
        loopNezDelay(headNops);

        // Timing critical point #1: >= 17 ticks after clock edge until read of address/control lines
        PROFILE1();
        uint32_t r0 = REG_READ(gpioInReg);
        // While that APB read is in flight (the core runs on until an
        // instruction needs r0), store the head's $4000 and $C000 blocks
        // into the table, so the decode below is a plain table load: four
        // instructions fewer between the address and extSel_ (bench: P1 to
        // extSel_ ~4 cycles shorter; the stores themselves cost nothing,
        // hidden in the read). Stored before P1 instead, they stalled the
        // read (~5 cycles on the bench).
        // mmuState.l1Blocks[l1Block40] and [l1BlockC0] are then current,
        // though core 0 code still reads those two through mmuBlock().
        __asm__ volatile ("" ::: "memory");   // the stores after the read
        mmuState.l1Blocks[l1Block40] = block40;
        mmuState.l1Blocks[l1BlockC0] = blockC0;

        // Release last cycle's data bus (and halt_/irq_ when core 0 has
        // lifted them; mmuState.pinReleaseMask changes at run time, so it's
        // loaded every cycle, at offset 0 from &mmuState). After P1: issued
        // before it, this APB store stalled the address read. Profile points
        // 4 and 6 (the read and write releases) both mark it.
        PROFILE4();
        PROFILE6();
        REG_WRITE(gpioEnableClrReg, mmuState.pinReleaseMask);
        __asm__ volatile ("" ::: "memory");   // keep the decode loads after it

        const int l1BlockIndex = ((r0 & bus.addr.mask) >> l1BlockSelShift);
        const int pageInL1Block = ((r0 & pageInL1BlockSelBits) >> pageSelShift)
              | ((r0 & bus.rw.mask) >> (bus.rw.shift - (pageBits - l1BlockBits)));
        // possibly remove this shift  ^^^ statement if RW pin is moved

        L1Block *block = mmuState.l1Blocks[l1BlockIndex];
        const uint32_t pageEn = block->ctrl[pageInL1Block];

        // Timing critical point #2: extSel_ (and mpd_) must be set up before
        // phi2 rises, where the Atari samples them; late, its own RAM/ROM
        // starts the cycle and fights the bus. So only the pin-enable lookup
        // comes before them: the barrier, which also renews r0, keeps the
        // data-pointer load and the address extract after the enable store.
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
            if constexpr (BankSelect == CartBankSelect::D500_DATA_BANK_SEL) {
                // MegaCart: select the bank from $D500's data here too, so
                // a bank write reaches the mapping after the next cycle, read
                // or write; the write path only sees it on the next write.
                // Free here: the read tail waits for phi2 anyway. $D500's
                // shadow is addressed from portbShadow's register (no l32r).
                mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[portbShadow[_0x500 - _0x301] & 0x1f];
            }
            mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(*portbShadow >> 1) & 0x1];
            ILOOP_TAIL_RISE_THEN_WAIT(read);
        } else {
            const int page = pageNr(addr);
            lastPageOffset[page] = addr;

            // Set cartridge bank index based on D500 write address or data, depending on scheme.
            // MegaCart's ($D500's data) is the previous write's: this cycle's
            // data isn't sampled yet. One cycle late with the read path's copy.
            const int cartBankIndex =
                (BankSelect == CartBankSelect::D500_ADDR_BANK_SEL)
                    ? (lastPageOffset[pageD5] & 0x1f)
                    : (portbShadow[_0x500 - _0x301] & 0x1f);
            mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];
            // Map it now, so the cart switch reaches the next cycle.
            mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(*portbShadow >> 1) & 0x1];
            const int bmonNext = bmonPush(r0);
            loopNezDelay(wrSampleNops);
            PROFILE5();
            uint32_t r1 = REG_READ(GPIO_IN1_REG);
            data = (r1 >> bus.data.shift);
            *ramAddr = data;
            bmonPublish(bmonNext);
            ILOOP_TAIL_POLL(write);
        }
    }
waitFall:
    ILOOP_WAIT_FALL();
}

template void iloop_pbi<CartBankSelect::D500_ADDR_BANK_SEL>();
template void iloop_pbi<CartBankSelect::D500_DATA_BANK_SEL>();

#endif  // CSIM
