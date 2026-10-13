#pragma once

#include "esp_attr.h"

#define CORE0_HOTSPOTS_EXPLICIT_PLACEMENT 0

#if CORE0_HOTSPOTS_EXPLICIT_PLACEMENT
#define CORE0_HOTSPOTS_ATTR __attribute__((section(".text.core0_hotspots"), noinline))
#else
#define CORE0_HOTSPOTS_ATTR IRAM_ATTR __attribute__((noinline))
#endif
