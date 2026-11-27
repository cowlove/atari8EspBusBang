// Main's second-stage bootloader hook: the UART0 pads off the Atari's data
// bus as early as software can (AGENTS.md, boot-time pin hazards).
//
// GPIO43 and GPIO44 are Atari D5 and D6, and their reset function is
// U0TXD/U0RXD. With the ROM log fuse burned and no UART console the chip
// shouldn't drive them, but this makes sure, before anything else in the
// bootloader runs: GPIO function, output enable off, input on, no pulls.
// The ROM's part of the boot (before this hook) can't be changed in
// software.
//
// GPIO48 too: on the pool's espBusBang boards it is R1 to the Atari's
// RESET (2026-11-27: a 6502 parked in native RAM came out of main's
// restart running the OS, as if reset, on the 800XLs). Its state and
// level are recorded, then it is made a plain input as well.
//
// And the bus, sampled for a few ms before any of that (either main board
// pin map, below; phi2 GPIO36): whether the clock ran, whether the 6502 was still in the sled test's park loop ($3000-$3002), whether
// it wrote (the park loop doesn't), whether GPIO48 read low.
//
// What the pads were set to on entry is passed to the app in
// RTC_CNTL_STORE0_REG ("reserved" in the ROM's store-register table; unused
// by IDF 6.1): kBootPinsMagic in the top 16 bits, then per pad (GPIO43 bits
// 0-3, GPIO44 bits 4-7, GPIO48 bits 8-11): output enabled, function
// (MCU_SEL) not GPIO, pull-up on, pull-down on; bit 12 GPIO48 high at
// entry, bit 13 phi2 ran, bit 14 the park loop seen, bit 15 a write or
// GPIO48 low seen (bus sample). Main reports it (bootPinsReport(),
// main.cpp) and warns if 43/44 was an output.
#include <stdint.h>
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/rtc_cntl_reg.h"

#define A8BB_BOOT_PINS_MAGIC 0xa8b0u

void bootloader_hooks_include(void) {}   // makes the linker keep this file

static uint32_t padState(int gpio, uint32_t muxReg)
{
    const uint32_t mux = REG_READ(muxReg);
    const uint32_t oe = (REG_READ(GPIO_ENABLE1_REG) >> (gpio - 32)) & 1;
    const uint32_t notGpio = ((mux >> MCU_SEL_S) & MCU_SEL_V) != PIN_FUNC_GPIO;
    const uint32_t wpu = (mux & FUN_PU) != 0, wpd = (mux & FUN_PD) != 0;
    return oe | notGpio << 1 | wpu << 2 | wpd << 3;
}

static void padToInput(int gpio, uint32_t muxReg)
{
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, 1u << (gpio - 32));
    PIN_FUNC_SELECT(muxReg, PIN_FUNC_GPIO);
    PIN_PULLUP_DIS(muxReg);
    PIN_PULLDWN_DIS(muxReg);
    PIN_INPUT_ENABLE(muxReg);
}

// The bus, sampled: bit 13 phi2 ran, 14 the park loop ($3000-$3002) the
// most of the cycles' reads, 15 a write or GPIO48 low seen.
static uint32_t busSample(void)
{
    // Both main board pin maps (main/pinDefs.h, -DBOARD_VERSION; the
    // bootloader is built without it): version 1, A0-A15 on GPIO1-16 and
    // R/W on GPIO17; version 2, A0-A13 on GPIO1-14, R/W on GPIO15, A14-A15
    // on GPIO16-17. Each map's decode of the other's park-loop reads is a
    // write (R/W reads A14 or A15, 0 at $3000), so the park counts don't
    // cross; a board's writes are the fewer of the two maps' counts.
    uint32_t cycles = 0, park1 = 0, park2 = 0, writes1 = 0, writes2 = 0, resetLow = 0;
    int high = 0;
    // Input on for the pads sampled (only FUN_IE: nothing driven, the
    // function and pulls left alone). A pad whose input is off at reset
    // reads 0: on board version 2, R/W (GPIO15, also XTAL_32K_P) read as a
    // write on every cycle, so the park was never seen there (.3, .4),
    // while main's own probe in setup() saw it.
    for (int g = 1; g <= 17; g++) PIN_INPUT_ENABLE(PERIPHS_IO_MUX_GPIO0_U + 4 * g);   // IO_MUX_GPIOn_REG
    PIN_INPUT_ENABLE(IO_MUX_GPIO36_REG);
    PIN_INPUT_ENABLE(IO_MUX_GPIO48_REG);
    for (int i = 0; i < 40000; i++) {
        const uint32_t in1 = REG_READ(GPIO_IN1_REG), in0 = REG_READ(GPIO_IN_REG);
        resetLow += (in1 & (1u << (48 - 32))) == 0;
        const int clk = (in1 & (1u << (36 - 32))) != 0;
        if (clk && !high) {
            cycles++;
            const uint32_t addr1 = (in0 >> 1) & 0xffff;
            const uint32_t addr2 = ((in0 >> 1) & 0x3fff) | ((in0 >> 16) & 3) << 14;
            if ((in0 & (1u << 17)) == 0) writes1++;
            else if (addr1 - 0x3000u < 3u) park1++;
            if ((in0 & (1u << 15)) == 0) writes2++;
            else if (addr2 - 0x3000u < 3u) park2++;
        }
        high = clk;
    }
    const uint32_t park = park1 > park2 ? park1 : park2;
    const uint32_t writes = writes1 < writes2 ? writes1 : writes2;
    return (cycles > 100) << 13 | (park * 4 > cycles && cycles > 100) << 14 | (writes > 0 || resetLow > 0) << 15;
}

// Main's RESTART_PAD_HOLD restart holds the bus pads through the reset
// (main.cpp, kHeldPads): make each a plain input (GPIO function, input on,
// no pulls, no output enable), as main left it, then release the holds,
// so nothing on the bus changes when they go.
static void busPadsRelease(void)
{
    static const uint8_t pads[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 21,
                                   35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48};
    uint32_t rtc = 0, dig = 0;
    for (unsigned i = 0; i < sizeof(pads); i++) {
        const int g = pads[i];
        if (g <= 21) rtc |= 1u << g;
        else dig |= 1u << (g - 21);
    }
    if ((REG_READ(RTC_CNTL_PAD_HOLD_REG) & rtc) == 0 && (REG_READ(RTC_CNTL_DIG_PAD_HOLD_REG) & dig) == 0)
        return;   // not held: leave the pads as the ROM has them
    for (unsigned i = 0; i < sizeof(pads); i++) {
        const int g = pads[i];
        const uint32_t mux = PERIPHS_IO_MUX_GPIO0_U + 4 * g;   // IO_MUX_GPIOn_REG
        if (g < 32) REG_WRITE(GPIO_ENABLE_W1TC_REG, 1u << g);
        else REG_WRITE(GPIO_ENABLE1_W1TC_REG, 1u << (g - 32));
        PIN_FUNC_SELECT(mux, PIN_FUNC_GPIO);
        PIN_PULLUP_DIS(mux);
        PIN_PULLDWN_DIS(mux);
        PIN_INPUT_ENABLE(mux);
    }
    REG_CLR_BIT(RTC_CNTL_PAD_HOLD_REG, rtc);
    REG_CLR_BIT(RTC_CNTL_DIG_PAD_HOLD_REG, dig);
}

void bootloader_before_init(void)
{
    const uint32_t bus = busSample();
    const uint32_t s43 = padState(43, IO_MUX_GPIO43_REG), s44 = padState(44, IO_MUX_GPIO44_REG);
    const uint32_t s48 = padState(48, IO_MUX_GPIO48_REG);
    const uint32_t level48 = (REG_READ(GPIO_IN1_REG) >> (48 - 32)) & 1;
    padToInput(43, IO_MUX_GPIO43_REG);
    padToInput(44, IO_MUX_GPIO44_REG);
    padToInput(48, IO_MUX_GPIO48_REG);
    busPadsRelease();
    REG_WRITE(RTC_CNTL_STORE0_REG,
              A8BB_BOOT_PINS_MAGIC << 16 | bus | level48 << 12 | s48 << 8 | s44 << 4 | s43);
}
