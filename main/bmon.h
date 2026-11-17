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
// The read ring (below) is smaller: best effort, and only needs to hold a
// wedged 6502's loop for the exit dump past an ANTIC DMA burst (a text
// mode badline: ~80 DMA reads plus refresh in a 114-cycle line; 256 is
// ~2 lines). The write ring's size is core 0's lap margin (2-3 ms of
// 6502 writes), so it stays 512.
static const DRAM_ATTR unsigned int bmonRdArraySz = 256;
static_assert((bmonRdArraySz & (bmonRdArraySz - 1)) == 0, "bmonRdArraySz must be a power of 2");
static const DRAM_ATTR unsigned int bmonRdArraySzMask = bmonRdArraySz - 1;
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
//
// Two rings (2026-11-17, the user's design), so reads can't flush a write:
// a halted 6502's bus still carries reads (idle cycles, DMA, refresh), and
// in one ring a late core 0 could find the halting write lapped. The write
// it must act on is always the newest write (a halted 6502 writes nothing
// more), so in a ring of writes nothing pushes it out.
//   writes: bmonArray, bmonHead, bmonTail: everything core 0 acts on
//           ($D301/$D1FF halts, $D5xx cart, $D830/$D840 requests,
//           $D5B9 R-Time 8 writes; the stackprog wait; the first-write
//           handshake at boot).
//   reads:  bmonRdArray, bmonRdHead, bmonRdTail: best effort, drained only
//           when the write ring is empty; only R-Time 8's $D5B9 reads use
//           it (a lost or late one breaks one clock read, which the SDX
//           driver's double read and compare tolerates). Reads after a
//           write can be handled before it only if core 0 is behind.
// Heads are free-running (core 1 masks to index, as core 0 does); a ring
// whose head is more than bmonArraySz past its tail lapped, and core 0
// skips to its newest entries. The arrays and heads live in core 1's hot
// struct (Core1Hot, mmu.h), which defines their names.
struct BmonEntry {
    uint32_t r0;
};
extern unsigned int bmonTail;     // core 0's, writes
extern unsigned int bmonRdTail;   // core 0's, reads
extern unsigned int bmonLaps, bmonRdLaps;   // laps seen (entries lost)

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

#include "mmu.h"   // Core1Hot: bmonArray, bmonHead, bmonRdArray, bmonRdHead
