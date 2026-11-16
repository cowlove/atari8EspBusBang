#pragma once
#include <inttypes.h>
#include "esp_attr.h"
#include "core0_hotspots.h"

struct __attribute__((packed)) PbiIocb {
    uint8_t req;
    uint8_t cmd;
    uint8_t a;
    uint8_t x;

    uint8_t y;
    uint8_t carry;
    uint8_t result;
    uint8_t psp;

    uint16_t copybuf;
    uint16_t copylen;

    uint8_t kbcode;
    uint8_t sdmctl;
    uint8_t stackprog;
    uint8_t consol;
};

IRAM_ATTR int handlePbiRequest2(PbiIocb *pbiRequest);
extern int haltLineDetected;            // -1 untested, 0 no, 1 yes
#ifdef ATARI800_SIM
extern "C" CORE0_HOTSPOTS_ATTR void handlePbiRequest(PbiIocb *pbiRequest);
#else
CORE0_HOTSPOTS_ATTR void handlePbiRequest(PbiIocb *pbiRequest);
#endif

struct MmuState;   // mmu.h
IRAM_ATTR uint8_t *mappedElseCopyIn(PbiIocb *pbiRequest, uint16_t addr, uint16_t len); 
// Scatter/gather between an SIO request's 6502 buffer [addr, addr + len)
// and ESP memory, for a buffer the board maps in pieces (base RAM pieces,
// 4 KB ext bank chunks) or only partly: the mapped runs from addr are
// copied directly; from the first page the board doesn't map, the rest goes
// through COPYBUF (pbiROM[0x400]) in one copy-in or copy-out, which the PBI
// ROM's copy buffer always holds (len <= REQ_MAX_COPYLEN). ms: the mapping
// to use (mmuStateSaved with the bus detached, mmuState while attached).
// allCopied: the whole buffer through COPYBUF (csim's coverage of that path).
//   mappedElseCopyIn(..., run): the piece at addr: COPYBUF if a copy-in
//     already holds [addr, addr + len), else the mapped run (*run bytes),
//     else nullptr with RES_FLAG_NEED_COPYIN set for [addr, addr + len).
//   pbiGather(): the buffer into dst; false (RES_FLAG_NEED_COPYIN set) if a
//     copy-in must come first: return that, and the re-issued request,
//     REQ_FLAG_COPYIN set, gathers it all.
//   pbiScatter(): src into the buffer; RES_FLAG_COPYOUT (copybuf/copylen
//     naming the rest, its data in COPYBUF) if part of it isn't mapped,
//     else RES_FLAG_COMPLETE.
IRAM_ATTR uint8_t *mappedElseCopyIn(MmuState &ms, PbiIocb *pbiRequest, uint16_t addr, uint16_t len,
                                    uint16_t *run, bool allCopied);
IRAM_ATTR bool pbiGather(MmuState &ms, PbiIocb *pbiRequest, uint16_t addr, uint16_t len, uint8_t *dst,
                         bool allCopied);
IRAM_ATTR int pbiScatter(MmuState &ms, PbiIocb *pbiRequest, uint16_t addr, uint16_t len, const uint8_t *src,
                         bool allCopied);
IRAM_ATTR void dumpScreenToSerial(char tag, uint8_t *mem = NULL);
