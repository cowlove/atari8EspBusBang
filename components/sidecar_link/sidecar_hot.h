#pragma once
/* Placement and pacing for the i2slink polling loops.
 *
 * On the host these loops run on core 0 alongside the core 1 bus loop, so
 * they must run from IRAM (never flash) and must not hammer SRAM with
 * ifetch and polling reads. */

#include "esp_attr.h"
#include "sdkconfig.h"

#include <stdint.h>

/* The host's Core0 hotspot placement (mirrors main/core0_hotspots.h, which
 * this component can't include), for the tight polling loops only
 * (i2slink_read(), i2slink_peek(), i2slink_hint()); the rest of the call chain is plain
 * IRAM_ATTR. With CONFIG_A8BB_CORE0_HOTSPOTS_EXPLICIT_PLACEMENT, main/link.lf
 * maps libsidecar_link.a's core0_hotspots section to IRAM; the Sidecar app
 * has no such option and gets plain IRAM_ATTR. */
#if CONFIG_A8BB_CORE0_HOTSPOTS_EXPLICIT_PLACEMENT
#define SIDECAR_HOT_ATTR __attribute__((section(".text.core0_hotspots"), noinline))
#else
#define SIDECAR_HOT_ATTR IRAM_ATTR __attribute__((noinline))
#endif

/* Iterations of sidecar_nez_delay() per idle poll: about two cycles each,
 * so 10 is about 0.08 us at 240 MHz. The tripwire window is about 24 us, so
 * even a few hundred costs nothing in window hits. */
#ifndef SIDECAR_POLL_DELAY
#define SIDECAR_POLL_DELAY 200U
#endif

/* Simulation hooks (csim/a800 builds this component on the host): what
 * the link's loops cost in CPU time, which simulated time has no other way
 * to see. Nothing on the chip. A lock search's cost was measured on the
 * a24 bench pair (2026-11-14): lockstress peek recoveries, one anchored
 * search plus a copy, average 253 us; the exhaustive search took about
 * 2.4 ms before the anchored one existed. */
#ifndef SIDECAR_SIM_CPU_US
#define SIDECAR_SIM_CPU_US(us) ((void)0)
#endif
#define SIDECAR_SEARCH_US_ANCHORED 250U
#define SIDECAR_SEARCH_US_FULL 2400U

#if !defined(__XTENSA__)
/* A host build (csim): the delay is simulated time, provided there. */
#ifdef __cplusplus
extern "C"
#endif
void sidecar_sim_delay_cycles(uint32_t cycles);
static inline void sidecar_nez_delay(uint32_t iterations)
{
    sidecar_sim_delay_cycles(2U * iterations);
}
#else
/* A delay with no instruction fetch: a zero-overhead loopnez over two
 * nop.n, aligned so the 4-byte body sits in one fetch word and is never
 * refetched (a C copy of loopNezDelay() in main/util.h). */
static inline __attribute__((always_inline)) void sidecar_nez_delay(uint32_t iterations)
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
