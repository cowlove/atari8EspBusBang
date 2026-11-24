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

// The decode (pinDefs.h has the board's pin map). The block is A14-A15,
// addrHi. The page's table index is A8-A13 (the page in its block) with R/W
// as PAGESEL_RD (bit 6). On the carrier R/W sits right above A13, so one
// extract takes both (rwAbovePage); on espBusBang R/W (GPIO17) is shifted
// down into place: three more instructions in every cycle's head.
static constexpr int pageSelShift = bus.addrLo.shift + 8;   // A8's bit in r0
static constexpr bool rwAbovePage = bus.rw.shift == pageSelShift + (pageBits - l1BlockBits);
static_assert(PAGESEL_RD == (1 << (pageBits - l1BlockBits)), "R/W is the index bit above the page");
static_assert(bus.addrHi.bits == l1BlockBits && bus.addrLo.bits == 16 - l1BlockBits,
              "addrHi is the block number");

// The board's own timing offsets, added to hwType.h's per-machine delays.
// The carrier's shorter decode (rwAbovePage) asserts extSel_/mpd_ (P2) 4
// cycles earlier: that is the gain. The address read (P1), the read data
// drive (P3) and the write data sample (P5) stay where they were tuned on
// espBusBang (static counts, tools/core1_cycle_count.py: P1 11, P3 38, P5
// 69 D500_ADDR / 70 D500_DATA): without these the carrier's P1 lands at 10
// (head scheduling) and P3/P5 4 earlier, untested timing.
#if CONFIG_A8BB_BOARD_CARRIER
static constexpr int boardHeadNops = 1, boardRdDataNops = 3, boardWrSampleNops = 3;
#else
static constexpr int boardHeadNops = 0, boardRdDataNops = 0, boardWrSampleNops = 0;
#endif

// Cycle timing (headNops, rdDataNops, wrSampleNops) comes straight from
// hwType.h's per-machine table, each an exact cycle count for
// loopNezDelay() (util.h).

// The D500_ADDR scheme's cart bank (0-31), from the last write to
// $D500-$D51F (core 1 hot data: .bss, mmu.h). A Maxflash 1 Mbit cart
// decodes only that range, as atari800's ATMAX_128 does, so a write to
// $D520-$D5FF must leave it alone: SDX's store to $D5B9 (the R-Time 8
// clock's registers) once turned the cart off under SDX, which hung booting
// any D1 without an SDX boot sector. Core 0 saves and restores it around a
// bus disable (main.cpp), so it lives in memory, not a register.
CORE1_HOT_BSS uint8_t cartBankLatch;

// One write cycle's update; returns the bank. Branchless, one asm block so
// the byte goes through memory every write (core 0 can reach it) and the
// time to the data sample doesn't vary:
//   l8ui old ; extui t, r0, addr bit 5, 11 ; xor t, 0xd500 >> 5
//   extui v, r0, addr bit 0, 5 ; moveqz old, v, t ; s8i old
// From r0, not addr: addr is r0ToAddr(r0) as a uint16_t, which
// would cost a truncation.
//
// The field is A5-A15 as they sit in r0, from A5 up to the top address bit:
// on the carrier that spans R/W (between A13 and A14), always 0 on a write,
// so the compared value is $D500's own bits there (addrToR0()). It fits
// movi on espBusBang (xor); on the carrier ($CA8) it doesn't, and an l32r
// is out (core 1's literal pool), so it's subtracted with addmi + addi: one
// more cycle, on the write path only.
static constexpr int d5FieldStart = bus.addrLo.shift + 5;
static constexpr int d5FieldTop = (bus.addrHi.shift + bus.addrHi.bits > bus.addrLo.shift + bus.addrLo.bits)
                                      ? bus.addrHi.shift + bus.addrHi.bits : bus.addrLo.shift + bus.addrLo.bits;
static constexpr int d5FieldBits = d5FieldTop - d5FieldStart;
static constexpr int32_t d5Field = int32_t((addrToR0(0xd500) >> d5FieldStart) & ((1U << d5FieldBits) - 1));
static constexpr bool d5FieldFitsMovi = d5Field <= 2047;
// -d5Field as addmi (a multiple of 256) + addi (-128..127).
static constexpr int32_t d5NegLo = ((-d5Field % 256) + 256) % 256 > 127 ? ((-d5Field % 256) + 256) % 256 - 256
                                                                        : ((-d5Field % 256) + 256) % 256;
static constexpr int32_t d5NegHi = -d5Field - d5NegLo;
static_assert(d5NegHi % 256 == 0 && d5NegHi >= -32768 && d5NegHi <= 32512, "addmi range");
static_assert(bus.addrLo.bits >= 5, "A0-A4 in addrLo");

static inline __attribute__((always_inline)) uint32_t cartBankLatchUpdate(uint32_t r0)
{
    constexpr int bit0 = bus.addrLo.shift;   // address bit 0's position in r0
#ifdef __XTENSA__
    uint32_t bank, t, v;
    if constexpr (d5FieldFitsMovi) {
        __asm__ volatile(
            "l8ui %0, %3, 0\n\t"
            "extui %1, %4, %6, %8\n\t"
            "xor %1, %1, %5\n\t"
            "extui %2, %4, %7, 5\n\t"
            "moveqz %0, %2, %1\n\t"
            "s8i %0, %3, 0"
            : "=&r"(bank), "=&r"(t), "=&r"(v)
            : "r"(&cartBankLatch), "r"(r0), "r"(d5Field), "i"(d5FieldStart), "i"(bit0), "i"(d5FieldBits));
    } else {
        __asm__ volatile(
            "l8ui %0, %3, 0\n\t"
            "extui %1, %4, %5, %8\n\t"
            "addmi %1, %1, %6\n\t"
            "addi %1, %1, %7\n\t"
            "extui %2, %4, %9, 5\n\t"
            "moveqz %0, %2, %1\n\t"
            "s8i %0, %3, 0"
            : "=&r"(bank), "=&r"(t), "=&r"(v)
            : "r"(&cartBankLatch), "r"(r0), "i"(d5FieldStart), "i"(d5NegHi), "i"(d5NegLo),
              "i"(d5FieldBits), "i"(bit0));
    }
    return bank;
#else
    volatile uint8_t &latch = cartBankLatch;
    const uint32_t a = r0ToAddr(r0);
    if ((a & 0xffe0) == 0xd500) latch = a & 0x1f;
    return latch;
#endif
}

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
    GPIO_IN_REG = addrToR0(busAddr) | (busRead ? bus.rw.mask : 0) | notRefresh;
    GPIO_IN1_REG = busRead ? 0 : (uint32_t(*data) << bus.data.shift);

    // ---- one pass of iloop_pbi(), starting at the clock's falling edge ----

    // Release last cycle's extSel_ and mpd_.
    dedic_gpio_cpu_ll_write_mask(bus.ctrlExtSel.mask | bus.ctrlMpd.mask, 0);

    // Head: map the ext bank ($4000) and the OS/PBI ROM ($C000) from the
    // PORTB and $D1FF values the last write cycles left, so a switch written
    // last cycle applies to this one, as on the real MMU. They override
    // mmuState.l1Blocks[] for those two blocks, which isn't kept current.
    const uint8_t portb = d000Write[_0x301];
    const uint8_t d1ff = d000Write[_0x1ff];
    L1Block *const block40 = mmuState.extBanks[portb];
    L1Block *const blockC0 = mmuState.osEnL1BlockMux[(portb & 0x1) | (d1ff & pbiDeviceNumMask)];

    // P1: read the address and R/W lines.
    const uint32_t r0 = REG_READ(GPIO_IN_REG);

    // Release last cycle's data bus (and halt_/irq_ when core 0 has lifted
    // them): after the fall, and after P1 so the store doesn't delay it.
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, mmuState.pinReleaseMask);

    // Decode: the 16 KB block, and the page's entry in it (each page has
    // separate read and write entries).
    const uint16_t addr = r0ToAddr(r0);
    const bool read = (r0 & bus.rw.mask) != 0;
    const int page = pageNr(addr);
    const int l1BlockIndex = page2L1Block(page);
    const int pageInL1Block = (page & pageInL1BlockMask) | (read ? PAGESEL_RD : PAGESEL_WR) | PAGESEL_CPU;
    L1Block *block = mmuState.l1Blocks[l1BlockIndex];
    if (l1BlockIndex == l1Block40) block = block40;
    if (l1BlockIndex == l1BlockC0) block = blockC0;

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
        
#ifdef HAVE_REFRESH
        if ((r0 & bus.refresh_.mask) != 0)   // refresh cycles aren't recorded (as iloop_pbi())
#endif
        {
            const unsigned int bHead = bmonRdHead;   // the read ring (bmon.h)
            bmonRdArray[bHead & bmonRdArraySzMask].r0 = r0;
            bmonRdHead = bHead + 1;
        }
        
        // Tail: wait out phi1 if it is still low, then wait for the fall.
        while (dedic_gpio_cpu_ll_read_in() == 0) {}
        while (dedic_gpio_cpu_ll_read_in() != 0) {}
    } else {
        // The D500_ADDR scheme's bank: a write to $D500-$D51F selects it.
        const uint32_t addrBank = cartBankLatchUpdate(r0);

        // Cartridge bank select, from the $D5xx write's address (most
        // schemes) or, for MegaCart, $D500's data. MegaCart reads it before
        // this cycle stores the new data, so its switch takes effect after
        // the next cycle (the read path above picks it up too): one cycle
        // late. Mapping after the data sample would make it exact, at the
        // cost of the write path, the loop's longest.
        const int cartBankIndex = (atariCart.header.type == AtariCart::MegaCart128)
                                      ? (d000Write[_0x500] & 0x1f)
                                      : addrBank;
        mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];
        // Map it now, so the cart switch reaches the next cycle.
        mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(d000Write[_0x301] >> 1) & 0x1];

        // Record the write for core 0 (the address/control word from P1).
        const unsigned int bHead = bmonHead;   // the write ring (bmon.h)
        bmonArray[bHead & bmonArraySzMask].r0 = r0;

        // P5: sample the data lines and store the byte where the MMU maps
        // the page. Only then publish the bmon entry: core 0 acts on the
        // written value (d000Write[], a PBI request) as soon as it sees it.
        const uint32_t r1 = REG_READ(GPIO_IN1_REG);
        *ramAddr = r1 >> bus.data.shift;
        bmonHead = bHead + 1;

        // Tail: wait for the fall.
        while (dedic_gpio_cpu_ll_read_in() != 0) {}
    }

    // For csim's caller: did this board take the access, and what did it
    // drive?
    const bool extSelAsserted = (dedic_gpio_cpu_ll_read_out() & bus.ctrlExtSel.mask) != 0;
    if (extSelAsserted && read)
        *data = uint8_t(REG_READ(GPIO_OUT1_REG) >> bus.data.shift);
    CSIM_6502_YIELD();
    return extSelAsserted;
}

#else  // ESP32 core 1 from here on

// iloop_pbi(): each pass is one 6502 bus cycle, from the clock's falling
// edge: map the banks the last write selected, then serve the cycle (read:
// drive the data bus; write: latch it). r0 gets this cycle's GPIO_IN_REG.
// csim_bus_cycle() above is the same cycle in plain C++.
//
// bmonPush(): record a cycle's GPIO_IN_REG (address and control lines) for
// core 0, in its ring (bmon.h): writes in the write path's idle time before
// the data sample, reads in the read tail (which waits for phi2 anyway).
// bmonPush() writes the entry; bmonPublish() makes it visible to core 0,
// after the cycle's data store, since core 0 reads what was written
// (d000Write[], a PBI request) as soon as it sees the entry. Heads are
// free-running (masked here to index). Both rings and heads are in
// core1Hot, off the &mmuState register (mmu.h): no l32r, no register.
template <unsigned Mask>
static inline __attribute__((always_inline)) unsigned bmonPush(volatile unsigned int &head, BmonEntry *ring,
                                                               uint32_t r0) {
    const unsigned bHead = head;
    ring[bHead & Mask].r0 = r0;
    unsigned next = bHead + 1;
    __asm__ volatile ("" : "+r" (next));   // compute it here, not in the tail
    return next;
}
static inline __attribute__((always_inline)) void bmonPublish(volatile unsigned int &head, unsigned next) {
    __asm__ volatile ("" ::: "memory");   // after *ramAddr = data
    head = next;
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
    // core 1's hot struct (mmuState, the bmon rings and heads: mmu.h) through
    // one opaque pointer, so the rings' bases come off the same register as
    // mmuState (an addmi), not an l32r of core1Hot+offset each cycle.
    Core1Hot *hot = &core1Hot;
    __asm__ ("" : "+r" (hot));
#define mmuState (hot->mmu)
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
        // Load the remapped $4000 and $C000 blocks into registers; the decode
        // below picks them by index. Storing them into mmuState.l1Blocks[] here
        // and loading back from it after the address read stalled ~5 cycles
        // between the address read (P1) and the pin enable (P2) on the bench.
        // mmuState.l1Blocks[l1Block40] and [l1BlockC0] aren't kept current:
        // core 0 code reads those two through mmuBlock() (mmu.h).
        // Don't store them into the table after P1 either (0f9b5e62, reverted):
        // the decode's load of a word core 1 has just stored stalls far longer
        // than the static count shows, on every $4000/$C000 access, and broke
        // .5 (130XE) within minutes: boot hangs, SDX config errors, compare
        // failures. Stores to the table with no load of the stored word, or
        // the same stores to a scratch array, ran clean (2026-11-12 A/B).
        L1Block *const block40 = mmuState.extBanks[portb];
        L1Block *const blockC0 = mmuState.osEnL1BlockMux[osSel];
        loopNezDelay(headNops + boardHeadNops);

        // Timing critical point #1: >= 17 ticks after clock edge until read of address/control lines
        PROFILE1();
        uint32_t r0 = REG_READ(gpioInReg);

        // Release last cycle's data bus (and halt_/irq_ when core 0 has
        // lifted them; mmuState.pinReleaseMask changes at run time, so it's
        // loaded every cycle, at offset 0 from &mmuState). After P1: issued
        // before it, this APB store stalled the address read. Profile points
        // 4 and 6 (the read and write releases) both mark it.
        PROFILE4();
        PROFILE6();
        REG_WRITE(gpioEnableClrReg, mmuState.pinReleaseMask);
        __asm__ volatile ("" ::: "memory");   // keep the decode loads after it

        const int l1BlockIndex = (r0 & bus.addrHi.mask) >> bus.addrHi.shift;
        int pageInL1Block;
        if constexpr (rwAbovePage)   // carrier: page in block and R/W, one extui
            pageInL1Block = (r0 >> pageSelShift) & (2 * PAGESEL_RD - 1);
        else                         // espBusBang: R/W shifted down to PAGESEL_RD
            pageInL1Block = ((r0 >> pageSelShift) & pageInL1BlockMask)
                  | ((r0 & bus.rw.mask) >> (bus.rw.shift - (pageBits - l1BlockBits)));

        L1Block *block = mmuState.l1Blocks[l1BlockIndex];
        // Branchless (GCC emits beqi branches for the ternaries).
        const int d40 = l1BlockIndex - l1Block40, dC0 = l1BlockIndex - l1BlockC0;
        __asm__ ("moveqz %0, %1, %2" : "+r" (block) : "r" (block40), "r" (d40));
        __asm__ ("moveqz %0, %1, %2" : "+r" (block) : "r" (blockC0), "r" (dC0));
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
        ramAddr = &pageData[(r0 >> bus.addrLo.shift) & pageOffsetMask];   // A0-A7

        const int isReadOp = ((r0 & bus.rw.mask) >> bus.rw.shift);
        if (__builtin_expect(isReadOp, 1)) {
            data = *ramAddr;
            loopNezDelay(rdDataNops + boardRdDataNops);
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
#ifdef HAVE_REFRESH
            // Refresh cycles (refresh_ low; ~8% of cycles) aren't recorded:
            // core 0 never used them. One bit-test branch (the user's call:
            // normally no branches in core 1, but the read tail waits for
            // phi2 anyway, which absorbs it), the push the fall-through.
            if (__builtin_expect((r0 & bus.refresh_.mask) != 0, 1))
#endif
                bmonPublish(hot->mmu.rdHead, bmonPush<bmonRdArraySzMask>(hot->mmu.rdHead, hot->bmonRd, r0));
            ILOOP_TAIL_RISE_THEN_WAIT(read);
        } else {
            // The D500_ADDR scheme's bank: a write to $D500-$D51F selects it.
            const uint32_t addrBank =
                (BankSelect == CartBankSelect::D500_ADDR_BANK_SEL) ? cartBankLatchUpdate(r0) : 0;

            // Set cartridge bank index based on D500 write address or data, depending on scheme.
            // MegaCart's ($D500's data) is the previous write's: this cycle's
            // data isn't sampled yet. One cycle late with the read path's copy.
            const int cartBankIndex =
                (BankSelect == CartBankSelect::D500_ADDR_BANK_SEL)
                    ? addrBank
                    : (portbShadow[_0x500 - _0x301] & 0x1f);
            mmuState.basicEnL1BlockMux[1] = mmuState.cartBanks[cartBankIndex];
            // Map it now, so the cart switch reaches the next cycle.
            mmuState.l1Blocks[l1Block80] = mmuState.basicEnL1BlockMux[(*portbShadow >> 1) & 0x1];
            const unsigned bmonNext = bmonPush<bmonArraySzMask>(hot->mmu.wrHead, hot->bmonWr, r0);
            loopNezDelay(wrSampleNops + boardWrSampleNops);
            // MegaCart's loop skips cartBankLatchUpdate()'s 7 cycles minus
            // the (free) mapping index: hold its P5 at 70, where it soaked.
            if constexpr (BankSelect == CartBankSelect::D500_DATA_BANK_SEL) loopNezDelay(6);
            PROFILE5();
            uint32_t r1 = REG_READ(GPIO_IN1_REG);
            data = (r1 >> bus.data.shift);
            *ramAddr = data;
            bmonPublish(hot->mmu.wrHead, bmonNext);
            ILOOP_TAIL_POLL(write);
        }
    }
waitFall:
    ILOOP_WAIT_FALL();
}
#undef mmuState


template void iloop_pbi<CartBankSelect::D500_ADDR_BANK_SEL>();
template void iloop_pbi<CartBankSelect::D500_DATA_BANK_SEL>();

#endif  // CSIM
