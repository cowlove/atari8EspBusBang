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
// What the pads were set to on entry is passed to the app in
// RTC_CNTL_STORE0_REG ("reserved" in the ROM's store-register table; unused
// by IDF 6.1): kBootPinsMagic in the top 16 bits, then per pad (GPIO43 bits
// 0-3, GPIO44 bits 4-7): output enabled, function (MCU_SEL) not GPIO,
// pull-up on, pull-down on. Main reports it (bootPinsReport(), main.cpp)
// and warns if one was an output.
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

void bootloader_before_init(void)
{
    const uint32_t s43 = padState(43, IO_MUX_GPIO43_REG), s44 = padState(44, IO_MUX_GPIO44_REG);
    padToInput(43, IO_MUX_GPIO43_REG);
    padToInput(44, IO_MUX_GPIO44_REG);
    REG_WRITE(RTC_CNTL_STORE0_REG, A8BB_BOOT_PINS_MAGIC << 16 | s44 << 4 | s43);
}
