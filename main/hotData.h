#pragma once

// Print which internal SRAM bank each core's hot data sits in and flag any
// bank the core 1 bus loop shares with core 0's busiest data (see
// hotData.cpp). Call once everything is allocated and core 1 has started.
#ifndef CSIM
void reportHotDataBanks();
#else
static inline void reportHotDataBanks() {}
#endif
