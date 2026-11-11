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
// another, so each one moves only its own point and the ones after it on
// the same path. The P-points are tools/core1_cycle_count.py's.
//
//   headNops        delay before the address read (P1). 1 on every row since
//                   the $4000/$C000 table stores moved behind P1 (core1.cpp):
//                   the head got one instruction shorter, and this keeps P1
//                   where the 2026-11-11 soak had it (R/W settles at ~18-22
//                   ticks; an earlier P1 is untested).
//   rdDataNops      delay before the read data drive (P3)
//   wrSampleNops    delay before the write-data sample (P5). 10 on every
//                   row since 2026-11-11: GPIO profiles put the last write-
//                   data change at 78-88 ticks on .1-.5, with no pattern by
//                   model (the 130XE settles earliest), so 10 (P5 69, as
//                   .1/.2 ran) for all: .3/.4 sample 2 cycles later than
//                   before, .5/.6 8 earlier. Where P5's read actually
//                   latches on hardware is still unmeasured.
//
// The bus releases have no knob: extSel_/mpd_ go right after the clock fall
// and the data bus right after this cycle's extSel_ assert (P4/P6).
//
// Each is loopNezDelay(n) (util.h): exactly n cycles, as nothing (0),
// n nop.n (1-3), or a loopnez of n - 3 passes plus its 3-cycle setup.
//
// Each can be overridden for one build with -D<NAME>=<cycles>, e.g.
// -DWR_SAMPLE_NOPS=10 (HEAD_NOPS, RD_DATA_NOPS, WR_SAMPLE_NOPS); that
// replaces the table's value.
struct HwTypeTiming {
    int headNops;
    int rdDataNops;
    int wrSampleNops;
};

// Tune by editing this table: one row per HW_TYPE, never shared, so each
// machine's settings can be read straight across and compared down the
// columns. The trailing comment is the row's static count
// (tools/core1_cycle_count.py, D500_ADDR) and what the row is based on.
struct HwTypeRow {
    const char *hwType;
    HwTypeTiming timing;
};

constexpr HwTypeRow hwTypeTable[] = {
//   HW_TYPE        head  rdData  wrSample      static P1 / P2 / P3 / P5, basis
    {"800XL",      {   1,      0,       10}},  // 11 / 26 / 35 / 69  .1/.2's tuning; not run as 800XL
    {"600XL",      {   1,      0,       10}},  // 11 / 26 / 35 / 69  .3/.4's tuning; not run as 600XL
    {"130XE",      {   1,      0,       10}},  // 11 / 26 / 35 / 69  as TESTPOOL5; not run as 130XE
    {"TESTPOOL1",  {   1,      0,       10}},  // 11 / 26 / 35 / 69  800XL: wr sample +2 (.1 compare failures when P5 moved earlier)
    {"TESTPOOL2",  {   1,      0,       10}},  // 11 / 26 / 35 / 69  800XL: as TESTPOOL1
    {"TESTPOOL3",  {   1,      0,       10}},  // 11 / 26 / 35 / 69  600XL: wr sample 8 -> 10 (unified)
    {"TESTPOOL4",  {   1,      0,       10}},  // 11 / 26 / 35 / 69  600XL: as TESTPOOL3
    {"TESTPOOL5",  {   1,      0,       10}},  // 11 / 26 / 35 / 69  130XE: wr sample 18 -> 10 (unified)
    {"TESTPOOL6",  {   1,      0,       10}},  // 11 / 26 / 35 / 69  130XE: as TESTPOOL5
};

constexpr HwTypeTiming hwTypeTiming() {
    for (const HwTypeRow &row : hwTypeTable)
        if (stringEquals(HW_TYPE, row.hwType)) return row.timing;
    return {-1, -1, -1};
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
#ifndef WR_SAMPLE_NOPS
#define WR_SAMPLE_NOPS HW_TYPE_TIMING.wrSampleNops
#endif
static constexpr int headNops = HEAD_NOPS;
static constexpr int rdDataNops = RD_DATA_NOPS;
static constexpr int wrSampleNops = WR_SAMPLE_NOPS;
// A negative loopnez count would spin ~2^32 times and hang core 1.
static_assert(headNops >= 0 && rdDataNops >= 0 && wrSampleNops >= 0,
              "core 1 timing values must be >= 0");
