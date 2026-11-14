#pragma once
// CSIM: the CPU cycle counter is the simulated CCOUNT (csim.h).
#include "csim.h"
static inline uint32_t esp_cpu_get_cycle_count(void) { return XTHAL_GET_CCOUNT(); }
