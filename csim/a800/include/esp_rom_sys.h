#pragma once
// CSIM: a ROM busy wait is a wait in simulated time (the rest of the
// simulation runs meanwhile: csim_coroutine.h).
#include "csim.h"
static inline void esp_rom_delay_us(uint32_t us)
{
    csim_wait_until(CSIM_PEEK_CCOUNT64() + uint64_t(us) * 240U);
}
