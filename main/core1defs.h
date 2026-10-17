#pragma once 
#ifndef HW_TYPE
#define HW_TYPE "800XL"
#endif
static constexpr DRAM_ATTR int SWEEP=80;

constexpr bool hwTypeEquals(const char *left, const char *right) {
    while (*left && *right) {
        if (*left++ != *right++) return false;
    }
    return *left == *right;
}

constexpr int hwTypeProfile() {
    return hwTypeEquals(HW_TYPE, "1") || hwTypeEquals(HW_TYPE, "800XL") ? 1 :
        hwTypeEquals(HW_TYPE, "2") ? 2 :
        hwTypeEquals(HW_TYPE, "3") || hwTypeEquals(HW_TYPE, "600XL") ? 3 :
        hwTypeEquals(HW_TYPE, "4") ? 4 :
        hwTypeEquals(HW_TYPE, "5") || hwTypeEquals(HW_TYPE, "130XE") ? 5 :
        hwTypeEquals(HW_TYPE, "6") ? 6 : -1;
}

static constexpr int HW_TYPE_PROFILE = hwTypeProfile();
static_assert(HW_TYPE_PROFILE >= 1, "Unsupported HW_TYPE; use 1-6, 130XE, 600XL, or 800XL");

static constexpr int LEADIN_NOPS =
    HW_TYPE_PROFILE == 3 || HW_TYPE_PROFILE == 4 ? 3 :
    HW_TYPE_PROFILE == 5 || HW_TYPE_PROFILE == 6 ? 0 : 4;
static constexpr int RD_EXIT_TS =
    HW_TYPE_PROFILE == 1 || HW_TYPE_PROFILE == 2 ? 100 :
    HW_TYPE_PROFILE == 3 || HW_TYPE_PROFILE == 4 ? 93 : 105;
static constexpr int WR_EXIT_TS =
    HW_TYPE_PROFILE == 1 || HW_TYPE_PROFILE == 2 ? 80 :
    HW_TYPE_PROFILE == 3 || HW_TYPE_PROFILE == 4 ? 75 : 85;
