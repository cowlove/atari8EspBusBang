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
unsigned int bmonWrHwm = 0, bmonRdHwm = 0;
uint8_t core0Phase = PH_LOOP;
uint16_t core0PhaseAddr = 0;
BmonMaxTag bmonWrMaxTag = {}, bmonRdMaxTag = {};
uint32_t bmonWrDepths[bmonDepthBuckets] = {}, bmonRdDepths[bmonDepthBuckets] = {};
uint32_t bmonWrLastDrain = 0, bmonRdLastDrain = 0;
#ifdef FAKE_SIO
bool bmonFakeWrBacklog = false, bmonFakeRdBacklog = false;
unsigned int bmonFakeWrHwm = 0, bmonFakeRdHwm = 0, bmonFakeLaps = 0, bmonFakeRdLaps = 0;
#endif
unsigned int mmuChangeBmonMaxEnd = 0;
unsigned int mmuChangeBmonMaxStart = 0;
