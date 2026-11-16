#pragma once 
#//pragma GCC optimize("O1")

#include <stdint.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "pinDefs.h"

#pragma GCC diagnostic ignored "-Wunused-variable"

static const DRAM_ATTR int bmonR0Shift = 0;//8;
static const DRAM_ATTR unsigned int bmonArraySz = 512;  
static_assert((bmonArraySz & (bmonArraySz - 1)) == 0, "bmonArraySz must be a power of 2");
static const DRAM_ATTR unsigned int bmonArraySzMask = bmonArraySz - 1;
// The bus monitor ring: core 1 pushes an entry for every bus cycle (reads
// and writes, r0 = the GPIO word: address, R/W, refresh_), core 0 consumes
// them in core0_onBmonChange(). Nothing stops core 1 when the ring is full:
// it laps the tail and the oldest entries are gone.
//
// The rule core 0 lives by (a design requirement, not a convention): the
// bmon loop, core0_onBmonChange() and its trips into core0_utilityTasks(),
// must keep up with the ring and never drop an entry, with one exception:
// while it is inside handlePbiRequest(). There a request (a Sidecar round
// trip, a disk image, a stackprog) can take milliseconds, against the ring's
// ~0.3 ms at one entry per 6502 cycle, so entries are lost, and when it
// returns the backlog is discarded (bmonTail = bmonHead). That is safe only
// because of where the 6502 is: in our PBI device driver with interrupts
// masked, waiting on the request, so it makes none of the writes core 0
// must act on, above all the ones that halt it (a write to $D1FF or $D301
// asserts halt_, and only core 0's resume6502() releases it: a lost one
// leaves the 6502 halted for good).
//
// So anything new on core 0 that can run longer than a few hundred bus
// cycles outside handlePbiRequest() (a link poll, flash, a LOG) breaks the
// rule; and any new reason for the 6502 to write $D1FF/$D301 (or another
// write core 0 must see) while a request is in flight breaks the exception.
// resume6502() also counts on core 1 running bus cycles while it holds the
// release (15 ticks): on the hardware it always does, and csim's bus-cycle
// mode must too (atari800's CPU_CSIM_Tick() runs idle cycles for the ones
// the CPU didn't, or the release can fall between two bus cycles).
struct BmonEntry {
    uint32_t r0, r1;
};
extern BmonEntry bmonArray[bmonArraySz];   // core 1 hot data: .bss (mmu.h)
extern volatile unsigned int bmonHead;
extern unsigned int bmonTail;

extern DRAM_ATTR unsigned int bmonMax;
extern DRAM_ATTR unsigned int mmuChangeBmonMaxEnd;
extern DRAM_ATTR unsigned int mmuChangeBmonMaxStart;

struct BmonTrigger { 
    uint32_t mask;
    uint32_t value;
    uint32_t mark; 
    int depth;
    int preroll;
    int count;
    int skip;
};

static const DRAM_ATTR struct {
    uint32_t mask;
    uint32_t value;
} bmonExcludes[] = { 
#if 1
#ifdef HAVE_REFRESH
    {
        .mask = (bus.refresh_.mask) << bmonR0Shift,                            // ignore refresh bus traffic  
        .value = (0) << bmonR0Shift,
    },
#endif
    {
        .mask = (bus.rw.mask | (0xf400 << bus.addr.shift)) << bmonR0Shift,   // ignore reads from char map  
        .value = (bus.rw.mask | (0xe000 << bus.addr.shift)) << bmonR0Shift,
    },
//    {
//        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift,   // ignore reads from 0x00ff
//        .value = (pins.read.mask | (0x00ff << pins.addr.shift)) << bmonR0Shift,
//    },
    {
        .mask = (bus.rw.mask | (0xf800 << bus.addr.shift)) << bmonR0Shift,   // ignore reads from screen mem
        .value = (bus.rw.mask | (0xb800 << bus.addr.shift)) << bmonR0Shift,
    },
#endif
};

//DRAM_ATTR volatile vector<BmonTrigger> bmonTriggers = {
static DRAM_ATTR BmonTrigger bmonTriggers[] = {/// XXTRIG 
#if 0 //TODO why does this trash the bmon timings?
    { 
        .mask =  (((0 ? pins.read.mask : 0) | (0xff00 << pins.addr.shift)) << bmonR0Shift) | (0x00), 
        .value = (((0 ? pins.read.mask : 0) | (0xd500 << pins.addr.shift)) << bmonR0Shift) | (0x00),
        .mark = 0,
        .depth = 10,
        .preroll = 5,
        .count = INT_MAX,
        .skip = 0 // TODO - doesn't work? 
    },
#endif
#if 0 
    { 
        .mask =  (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (0             | (0xd301 << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 2,
        .preroll = 0,
        .count = INT_MAX,
        .skip = 0 // TODO - doesn't work? 
    },
#endif
#if 0 // TODO: too many bmonTriggers slows down IO and hangs the system
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (0xfffa << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (0xfffe << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (39968 << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (0xfffa << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (0xfffe << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (39968 << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
#endif
};

extern void core0_onBmonChange();
