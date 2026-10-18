#pragma once

#include "esp_attr.h"

#if CONFIG_A8BB_CORE0_HOTSPOTS_EXPLICIT_PLACEMENT
#define CORE0_HOTSPOTS_ATTR __attribute__((section(".text.core0_hotspots"), noinline))
#else
#define CORE0_HOTSPOTS_ATTR IRAM_ATTR __attribute__((noinline))
#endif
