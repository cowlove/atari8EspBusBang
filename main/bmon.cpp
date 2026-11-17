#ifdef CSIM
#include "csim.h"
#endif

#include "bmon.h"
#include "esp_attr.h"

// The rings and heads are in core1Hot (mmu.h).
unsigned int bmonTail = 0;
unsigned int bmonRdTail = 0;
unsigned int bmonLaps = 0, bmonRdLaps = 0;
unsigned int bmonMax = 0;
unsigned int mmuChangeBmonMaxEnd = 0;
unsigned int mmuChangeBmonMaxStart = 0;
