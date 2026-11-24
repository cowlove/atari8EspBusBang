#pragma once
#include <stdint.h>
// CSIM: forced in (-include) when a firmware C file is compiled as C++
// against csim's C++ stub headers (components/i2slink/src/i2slink.c).
#ifdef __cplusplus
#define _Static_assert static_assert
#endif
// The link's CPU-time annotations (i2slink_hot.h) become simulated time.
#ifdef __cplusplus
extern "C"
#endif
void i2slink_sim_delay_cycles(uint32_t cycles);
#ifdef __cplusplus
extern "C"
#endif
void i2slink_sim_elapse_cycles(uint32_t cycles);
#define I2SLINK_SIM_CPU_US(us) i2slink_sim_delay_cycles((uint32_t)(us) * 240U)
#define I2SLINK_SIM_ELAPSE(cycles) i2slink_sim_elapse_cycles((uint32_t)(cycles))
// CSIM_LINK_FULL_SEARCH=1: every read() lock search is the exhaustive one.
#ifdef __cplusplus
extern "C"
#endif
int i2slink_sim_full_search_only(void);
#define I2SLINK_SIM_FULL_SEARCH_ONLY() i2slink_sim_full_search_only()
