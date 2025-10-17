#pragma once 
#//pragma GCC optimize("O1")

#include <stdint.h>
#include <inttypes.h>
#include "esp_attr.h"
#include "pinDefs.h"

#pragma GCC diagnostic ignored "-Wunused-variable"

static const DRAM_ATTR int bmonR0Shift = 8;
static const DRAM_ATTR unsigned int bmonArraySz = 1024;  // must be power of 2
static const DRAM_ATTR unsigned int bmonArraySzMask = bmonArraySz - 1;
extern DRAM_ATTR uint32_t bmonArray[bmonArraySz];
extern volatile DRAM_ATTR unsigned int bmonHead;
extern DRAM_ATTR unsigned int bmonTail;

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
    {
        .mask = (bus.refresh_.mask) << bmonR0Shift,                            // ignore refresh bus traffic  
        .value = (0) << bmonR0Shift,
    },
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
