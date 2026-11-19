#pragma once
/* Placement and pacing for the i2slink polling loops.
 *
 * They can run while the other core does hard real-time work (in the
 * project this came from, a 6502 bus loop on core 1), so they run from IRAM
 * (never flash) and pace their polling, to stay off the shared SRAM. */

#include "esp_attr.h"
#include "sdkconfig.h"

#include <stdint.h>

/* Where the tight polling loops go (i2slink_read(), i2slink_peek(),
 * i2slink_hint()); the rest of the call chain is plain IRAM_ATTR. By
 * default IRAM_ATTR too. A project that places its hot code itself defines
 * I2SLINK_HOT_SECTION (e.g. ".text.core0_hotspots") for this component and
 * maps that section to IRAM with a linker fragment. */
#ifdef I2SLINK_HOT_SECTION
#define I2SLINK_HOT_ATTR __attribute__((section(I2SLINK_HOT_SECTION), noinline))
#else
#define I2SLINK_HOT_ATTR IRAM_ATTR __attribute__((noinline))
#endif

/* Iterations of i2slink_nez_delay() per idle poll: about two cycles each,
 * so 10 is about 0.08 us at 240 MHz. The tripwire window is about 24 us, so
 * even a few hundred costs nothing in window hits. */
#ifndef I2SLINK_POLL_DELAY
#define I2SLINK_POLL_DELAY 200U
#endif

/* Simulation hooks (a host-side simulator can build this component, as
 * this repo's csim/a800 does): what
 * the link's loops cost in CPU time, which simulated time has no other way
 * to see. Nothing on the chip. A lock search's cost was measured on the
 * a24 bench pair (2026-11-14): lockstress peek recoveries, one anchored
 * search plus a copy, average 253 us; the exhaustive search took about
 * 2.4 ms before the anchored one existed. */
#ifndef I2SLINK_SIM_CPU_US
#define I2SLINK_SIM_CPU_US(us) ((void)0)
#endif
#define I2SLINK_SEARCH_US_ANCHORED 250U
#define I2SLINK_SEARCH_US_FULL 2400U
/* CPU cycles passing inline, without letting the rest of the simulation
 * run (code inside a critical section, such as the region copy with its
 * interrupts masked): the emulated hardware still moves meanwhile. The
 * copy's cost per 16-bit word is an estimate (a load, a store, the wrap
 * test), not yet measured. */
#ifndef I2SLINK_SIM_ELAPSE
#define I2SLINK_SIM_ELAPSE(cycles) ((void)0)
#endif
#ifndef I2SLINK_COPY_CYCLES_PER_HALFWORD
#define I2SLINK_COPY_CYCLES_PER_HALFWORD 5U
#endif

#if !defined(__XTENSA__)
/* A host build (csim): the delay is simulated time, provided there. */
#ifdef __cplusplus
extern "C"
#endif
void i2slink_sim_delay_cycles(uint32_t cycles);
static inline void i2slink_nez_delay(uint32_t iterations)
{
    i2slink_sim_delay_cycles(2U * iterations);
}
#else
/* A delay with no instruction fetch: a zero-overhead loopnez over two
 * nop.n, aligned so the 4-byte body sits in one fetch word and is never
 * refetched. */
static inline __attribute__((always_inline)) void i2slink_nez_delay(uint32_t iterations)
{
    __asm__ volatile("j 2f\n\t"
                     ".align 4\n\t"
                     "2:\n\t"
                     "loopnez %0, 1f\n\t"
                     "nop.n\n\t"
                     "nop.n\n\t"
                     "1:\n\t"
                     : "+r"(iterations)
                     :
                     : "memory");
}
#endif
