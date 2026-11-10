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

// Core 1 bus-cycle timing, per machine. Every value is in CPU cycles and
// goes straight into the cycle code (main/core1.cpp); none is derived from
// another, so each one moves only its own point. To move one point and
// keep a later one where it is, take the same amount off the later one:
// e.g. 2 more wrSampleNops and 2 fewer wrReleaseDelay keep the write
// release (P6) put. The P-points are tools/core1_cycle_count.py's.
//
//   headNops        nop.n before the address read (P1)
//   rdDataNops      nop.n before the read data drive (P3)
//   rdReleaseDelay  loopNezDelay() after the read data drive, before the
//                   read release (P4)
//   wrSampleNops    nop.n before the write-data sample (P5)
//   wrReleaseDelay  loopNezDelay() after the write-data sample, before the
//                   write release (P6)
//
// loopNezDelay(n) takes about n + 2 cycles (the loopnez setup isn't
// subtracted); a delay of 0 emits no loop at all. nop.n is exactly one
// cycle each.
//
// Each can be overridden for one build with -D<NAME>=<cycles>, e.g.
// -DWR_RELEASE_DELAY=1 (HEAD_NOPS, RD_DATA_NOPS, RD_RELEASE_DELAY,
// WR_SAMPLE_NOPS, WR_RELEASE_DELAY); that replaces the table's value.
//
// History: the first version of this table (941261ba) reproduced 9d19c99: rdReleaseDelay 56 was 2 * rdExitDelay(16) + 24, wrReleaseDelay
// 9 was 2 * wrExitDelay(10) - 5 - wrSampleNops(6), and headNops 0 was
// leadinNops(4) + 6 - 11 head instructions, floored at 0. The +24 includes
// +6 measured with PROFILE_MODE=GPIO on miner6 #1 and #2: it puts the read
// extSel release one APB tick (~12.5 ns) after master's. wrSampleNops 6
// puts the bench PROFILE_MODE=PIN write sample at 90-92 ticks, over the
// ~88 ticks hardware write data takes to settle.
struct HwTypeTiming {
    int headNops;
    int rdDataNops;
    int rdReleaseDelay;
    int wrSampleNops;
    int wrReleaseDelay;
};

// Tune by editing this table. Static counts (tools/core1_cycle_count.py,
// D500_ADDR) for each row are noted beside it; master's legacy timing
// (leadin 4, rd 16, wr 10) counted P1 10, P2 24, P3 47, P4 92, P5 73, P6 76.
constexpr HwTypeTiming hwTypeTiming() {
    // 800XL class, miner6 .1/.2: hand-tuned 2026-11-10 from PROFILE_MODE=GPIO
    // (write release 6 earlier than the 9d19c99 default of 9). Sample at the
    // bench's 90-tick target. P1 11, P3 39, P4 102, P5 63, P6 72.
    //                            head rdData rdRelease wrSample wrRelease
    constexpr HwTypeTiming xl800 = {0,  0,     56,       6,       3};
    // 600XL class, miner6 .3/.4: hand-tuned 2026-11-10 from PROFILE_MODE=GPIO
    // (read and write release 8 earlier). P1 11, P3 39, P4 94, P5 63, P6 70.
    constexpr HwTypeTiming xl600 = {0,  0,     48,       6,       1};
    // 130XE class, miner6 .5/.6: not tuned on this code yet. A static match
    // to master's legacy timing: read release, write sample and write
    // release exactly (P4 92, P5 73, P6 76). P1 is 1 later than master's
    // 10 (it can't be earlier); P3 stays at 39, 8 earlier than master's 47,
    // which only adds setup time. P1 11, P3 39, P4 92, P5 73, P6 76.
    constexpr HwTypeTiming xe130 = {0,  0,     46,      16,       0};

    if (stringEquals(HW_TYPE, "800XL"))     return xl800;
    if (stringEquals(HW_TYPE, "TESTPOOL1")) return xl800;
    if (stringEquals(HW_TYPE, "TESTPOOL2")) return xl800;
    if (stringEquals(HW_TYPE, "600XL"))     return xl600;
    if (stringEquals(HW_TYPE, "TESTPOOL3")) return xl600;
    if (stringEquals(HW_TYPE, "TESTPOOL4")) return xl600;
    if (stringEquals(HW_TYPE, "130XE"))     return xe130;
    if (stringEquals(HW_TYPE, "TESTPOOL5")) return xe130;
    if (stringEquals(HW_TYPE, "TESTPOOL6")) return xe130;
    return {-1, -1, -1, -1, -1};
}

static constexpr HwTypeTiming HW_TYPE_TIMING = hwTypeTiming();
static_assert(HW_TYPE_TIMING.headNops >= 0,
              "Unsupported HW_TYPE; use TESTPOOL1-6, 130XE, 600XL, or 800XL");

#ifndef HEAD_NOPS
#define HEAD_NOPS HW_TYPE_TIMING.headNops
#endif
#ifndef RD_DATA_NOPS
#define RD_DATA_NOPS HW_TYPE_TIMING.rdDataNops
#endif
#ifndef RD_RELEASE_DELAY
#define RD_RELEASE_DELAY HW_TYPE_TIMING.rdReleaseDelay
#endif
#ifndef WR_SAMPLE_NOPS
#define WR_SAMPLE_NOPS HW_TYPE_TIMING.wrSampleNops
#endif
#ifndef WR_RELEASE_DELAY
#define WR_RELEASE_DELAY HW_TYPE_TIMING.wrReleaseDelay
#endif
static constexpr int headNops = HEAD_NOPS;
static constexpr int rdDataNops = RD_DATA_NOPS;
static constexpr int rdReleaseDelay = RD_RELEASE_DELAY;
static constexpr int wrSampleNops = WR_SAMPLE_NOPS;
static constexpr int wrReleaseDelay = WR_RELEASE_DELAY;
// A negative loopnez count would spin ~2^32 times and hang core 1.
static_assert(headNops >= 0 && rdDataNops >= 0 && rdReleaseDelay >= 0 &&
              wrSampleNops >= 0 && wrReleaseDelay >= 0,
              "core 1 timing values must be >= 0");
