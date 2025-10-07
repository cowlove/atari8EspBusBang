#include "bmon.h"
#include "esp_attr.h"

uint32_t bmonArray[bmonArraySz] = {0};
volatile unsigned int bmonHead = 0;
unsigned int bmonTail = 0;
unsigned int bmonMax = 0;
unsigned int mmuChangeBmonMaxEnd = 0;
unsigned int mmuChangeBmonMaxStart = 0;

