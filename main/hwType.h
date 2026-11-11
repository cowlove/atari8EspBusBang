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
//   headNops        delay before the address read (P1)
//   rdDataNops      delay before the read data drive (P3)
//   rdReleaseDelay  delay after the read data drive, before the read
//                   release (P4)
//   wrSampleNops    delay before the write-data sample (P5)
//   wrReleaseDelay  delay after the write-data sample, before the write
//                   release (P6)
//
// Each is loopNezDelay(n) (util.h): exactly n cycles, as nothing (0),
// n nop.n (1-3), or a loopnez of n - 3 passes plus its 3-cycle setup.
// (Before 2026-11-10 loopNezDelay(n) cost n + 3 and the *Nops fields were
// plain nop.n; the release delays below were raised by 3 to keep the code.)
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

// Tune by editing this table: one row per HW_TYPE, never shared, so each
// machine's settings can be read straight across and compared down the
// columns. The trailing comment is the row's static count
// (tools/core1_cycle_count.py, D500_ADDR) and what the row is based on.
// master's legacy timing (leadin 4, rd 16, wr 10) counted P1 10, P2 24,
// P3 47, P4 92, P5 73, P6 76.
struct HwTypeRow {
    const char *hwType;
    HwTypeTiming timing;
};

constexpr HwTypeRow hwTypeTable[] = {
//   HW_TYPE        head  rdData  rdRelease  wrSample  wrRelease      static P3 / P4 / P5 / P6, basis
    {"800XL",      {   0,      0,        60,        8,         6}},  // 34 / 101 / 64 / 72  .1/.2's tuning as of 8d28535f; not run as 800XL; rd +1, wr sample +2 keep P4/P5 with 0dfb4756
    {"600XL",      {   0,      0,        52,        8,         4}},  // 34 /  93 / 64 / 70  .3/.4's tuning as of 8d28535f; not run as 600XL; rd +1, wr sample +2 keep P4/P5 with 0dfb4756
    {"130XE",      {   0,      0,        50,       18,         0}},  // 34 /  91 / 73 / 76  static match to master; not run as 130XE; rd +1, wr sample +2 keep P4/P5 with 0dfb4756
    {"TESTPOOL1",  {   0,      0,        60,       10,         4}},  // 34 / 101 / 66 / 72  800XL: wr sample +2 (.1 compare failures since the split moved P5 earlier); rd +1, wr sample +2 keep P4/P5 with 0dfb4756
    {"TESTPOOL2",  {   0,      0,        60,       10,         4}},  // 34 / 101 / 66 / 72  800XL: wr sample +2 (.1 compare failures since the split moved P5 earlier); rd +1, wr sample +2 keep P4/P5 with 0dfb4756
    {"TESTPOOL3",  {   0,      0,        52,        8,         4}},  // 34 /  93 / 64 / 70  600XL: rd/wr release 8 earlier (GPIO profile); rd +1, wr sample +2 keep P4/P5 with 0dfb4756
    {"TESTPOOL4",  {   0,      0,        52,        8,         4}},  // 34 /  93 / 64 / 70  600XL: as TESTPOOL3; rd +1, wr sample +2 keep P4/P5 with 0dfb4756
    {"TESTPOOL5",  {   0,      0,        50,       18,         0}},  // 34 /  91 / 73 / 76  130XE: static match to master; boots; rd +1, wr sample +2 keep P4/P5 with 0dfb4756
    {"TESTPOOL6",  {   0,      0,        50,       18,         0}},  // 34 /  91 / 73 / 76  130XE: as TESTPOOL5, not run on this code; rd +1, wr sample +2 keep P4/P5 with 0dfb4756
};

constexpr HwTypeTiming hwTypeTiming() {
    for (const HwTypeRow &row : hwTypeTable)
        if (stringEquals(HW_TYPE, row.hwType)) return row.timing;
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
