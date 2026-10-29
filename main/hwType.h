#pragma once 
// Defines low-level timing and tuning parameters for Atari 8-bit hardware variants.
#ifndef HW_TYPE
#define HW_TYPE "800XL"
#endif
static constexpr DRAM_ATTR int SWEEP = /*SWEEP 105 -1 80*/80;

constexpr bool stringEquals(const char *left, const char *right) {
    while (*left && *right) {
        if (*left++ != *right++) return false;
    }
    return *left == *right;
}

struct HwTypeTiming {
    int leadinNops;
    int rdExitTs;
    int wrExitTs;
};

constexpr HwTypeTiming hwTypeTiming() {
    HwTypeTiming params = {-1, -1, -1};

    if (stringEquals(HW_TYPE, "800XL")) {
        params.leadinNops =   4;
        params.rdExitTs    = 100;
        params.wrExitTs    =  80;
    }

    else if (stringEquals(HW_TYPE, "600XL")) {
        params.leadinNops =   3;
        params.rdExitTs    =  93;
        params.wrExitTs    =  75;
    }

    else if (stringEquals(HW_TYPE, "130XE")) {
        params.leadinNops =   0;
        params.rdExitTs    = 105;
        params.wrExitTs    =  85;
    }

    else if (stringEquals(HW_TYPE, "TESTPOOL1")) {
        params.leadinNops =   4;
        params.rdExitTs    = 100;
        params.wrExitTs    =  80;
    }

    else if (stringEquals(HW_TYPE, "TESTPOOL2")) {
        params.leadinNops =   4;
        params.rdExitTs    = 100;
        params.wrExitTs    =  80;
    }

    else if (stringEquals(HW_TYPE, "TESTPOOL3")) {
        params.leadinNops =   3;
        params.rdExitTs    =  93;
        params.wrExitTs    =  75;
    }

    else if (stringEquals(HW_TYPE, "TESTPOOL4")) {
        params.leadinNops =   3;
        params.rdExitTs    =  93;
        params.wrExitTs    =  75;
    }

    else if (stringEquals(HW_TYPE, "TESTPOOL5")) {
        params.leadinNops =   0;
        params.rdExitTs    = 105;
        params.wrExitTs    =  85;
    }

    else if (stringEquals(HW_TYPE, "TESTPOOL6")) {
        params.leadinNops =   0;
        params.rdExitTs    = 105;
        params.wrExitTs    =  85;
    }

    return params;
}

static constexpr HwTypeTiming HW_TYPE_TIMING = hwTypeTiming();
static_assert(HW_TYPE_TIMING.leadinNops >= 0,
              "Unsupported HW_TYPE; use TESTPOOL1-6, 130XE, 600XL, or 800XL");
static constexpr int LEADIN_NOPS = HW_TYPE_TIMING.leadinNops;
static constexpr int RD_EXIT_TS = HW_TYPE_TIMING.rdExitTs;
static constexpr int WR_EXIT_TS = HW_TYPE_TIMING.wrExitTs;
