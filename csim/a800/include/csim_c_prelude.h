#pragma once
#include <stdint.h>
// CSIM: forced in (-include) when a firmware C file is compiled as C++
// against csim's C++ stub headers (components/sidecar_link/i2slink.c).
#ifdef __cplusplus
#define _Static_assert static_assert
#endif
// The link's CPU-time annotations (sidecar_hot.h) become simulated time.
#ifdef __cplusplus
extern "C"
#endif
void sidecar_sim_delay_cycles(uint32_t cycles);
#ifdef __cplusplus
extern "C"
#endif
void sidecar_sim_elapse_cycles(uint32_t cycles);
#define SIDECAR_SIM_CPU_US(us) sidecar_sim_delay_cycles((uint32_t)(us) * 240U)
#define SIDECAR_SIM_ELAPSE(cycles) sidecar_sim_elapse_cycles((uint32_t)(cycles))
