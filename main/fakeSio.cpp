// FAKE_SIO: see fakeSio.h.
#ifdef CSIM
#include "csim.h"
#endif
#include "fakeSio.h"

#ifdef FAKE_SIO
#include "asmDefs.h"
#include "main.h"
#include "mmu.h"
#include "pbi.h"

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_random.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

namespace {

// Requests, one at a time, at most one every kGapMs (each takes a few
// utility passes anyway: submit, then a re-issue per copy-in or detach).
#ifndef FAKE_SIO_GAP_MS
#define FAKE_SIO_GAP_MS 10
#endif
constexpr uint32_t kTicksPerMs = 240U * 1000U;
constexpr uint32_t kGapTicks = FAKE_SIO_GAP_MS * kTicksPerMs;
// csim: let the OS finish booting (its PBI init) before the first request.
#ifdef CSIM
constexpr int kStartSec = 3;
#else
constexpr int kStartSec = 0;
#endif
constexpr int kTimeoutSec = 10;   // a request not served by then: stuck
constexpr int kMaxRetries = 16;   // re-issues (detach-safe, copy-ins) per request

// Test buffers anywhere in [kBufLo, kBufHi), at any alignment, so they
// cross pages and the mmu's mapped/unmapped boundaries. Clear of the stack
// page (the bench's fake bus test page), the OS variables and DCB ($0200-
// $03FF), the bench screen at $0400 ($0400-$071F), and, in csim, the idle
// cartridge ($A000) and the screen the OS put below it.
constexpr uint16_t kBufLo = 0x0800;
constexpr uint16_t kBufHi = 0x9000;

constexpr int kDrives = 2;           // D1: (the Sidecar's, with the link), D2: (local)
constexpr int kSectors = 720;        // both test images: 720 sectors
constexpr int kMaxReports = 8;       // mismatches printed in full

enum class Phase { Start, Idle, Pending };
enum class Kind : uint8_t { Status, Write, Read };

struct Drive {
    int sectorSize = 0;   // from the status command; 0 until known
    bool absent = false;  // its status failed: not mounted here, not tested
    uint16_t gen[kSectors + 1] = {};   // last pattern generation written (0: never)
};

struct Op {
    Kind kind;
    int drive;      // 0-based
    int sector;
    uint16_t addr, len;
    uint16_t gen;
    uint8_t flags;  // REQ_FLAG_*
    int retries;
    int issuedSec;
    bool mapped;    // the buffer was mapped at submit (else native)
    bool dataCopiedIn;   // a copy-in of this op's buffer was asked for and done
};

struct Stats {
    uint32_t submitted, completed, failed, timeouts;
    uint32_t writes, reads, verified, mismatches, statuses;
    uint32_t copyIns, copyOuts, detachSafe, mappedOps, nativeOps;
    uint32_t badRetries;
    uint32_t protocol;   // the ESP's side of the copy protocol misbehaved
    uint32_t copyBufVerified;   // reads checked in COPYBUF before the copy-out
};

Phase phase = Phase::Start;
Op op;
Stats stats;
Drive drives[kDrives];
uint32_t lastDoneTicks = 0;
int lastProgressSec = 0;
uint16_t nextGen = 1;
uint32_t rngState = 0;
uint8_t *native = nullptr;   // the 6502's own RAM (unmapped pages)

// Core 0 runs the bus loop with interrupts masked, where printf() (its
// stdout lock) aborts: everything to print is queued here as plain records
// and printed by fakeSioFlush() from a safe place (handleSerialIO(), in a
// local request's interrupts-enabled window; the end-of-run report).
enum class MsgType : uint8_t { Status, Absent, Mismatch, Failed, Protocol, Timeout, GiveUp };
struct Msg {
    MsgType type;
    uint8_t kind, drive, mapped;
    uint16_t sector, addr, len, gen;
    uint8_t a, b, c;          // per type: status/carry/DSTATS/result/req...
    uint16_t at, copybuf, copylen;
    const char *what;         // a literal
    uint8_t got[8], expected[8];
};
constexpr int kMsgs = 16;
Msg msgs[kMsgs];
volatile uint32_t msgHead = 0, msgTail = 0, msgsDropped = 0;
volatile bool progressDue = false;

Msg *newMsg(MsgType type)
{
    if (msgHead - msgTail >= (uint32_t)kMsgs) {
        ++msgsDropped;
        return nullptr;
    }
    Msg *m = &msgs[msgHead % kMsgs];
    *m = {};
    m->type = type;
    return m;
}

void postMsg() { ++msgHead; }

PbiIocb *request() { return reinterpret_cast<PbiIocb *>(&pbiROM[0x30]); }
uint8_t *copyBuf() { return const_cast<uint8_t *>(&pbiROM[0x400]); }   // $DC00

uint32_t rng()
{
    uint32_t x = rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rngState = x;
}

// The 6502's view of address a: ESP memory where the mmu maps it (as core 1
// would serve the bus), else native RAM.
uint8_t *view(uint16_t a)
{
    uint8_t *mapped = mmuCheckRangeMapped(const_cast<MmuState &>(mmuState), a, 1);
    return mapped ? mapped : &native[a];
}

bool rangeMapped(uint16_t a, uint16_t len)
{
    return mmuCheckRangeMapped(const_cast<MmuState &>(mmuState), a, len) != nullptr;
}

// The deterministic pattern for (drive, sector, gen): byte i.
uint8_t pattern(int drive, int sector, uint16_t gen, int i)
{
    uint32_t x = (uint32_t)(drive + 1) * 0x9E3779B1U ^ (uint32_t)sector * 0x85EBCA77U ^
                 (uint32_t)gen * 0xC2B2AE3DU ^ (uint32_t)i * 0x27D4EB2FU;
    x ^= x >> 15;
    x *= 0x2C1B3C6DU;
    x ^= x >> 12;
    return (uint8_t)x;
}

int sectorBytes(int drive, int sector)
{
    return sector <= 3 ? 128 : drives[drive].sectorSize;
}

void submit()
{
    AtariDCB *dcb = atariMem.dcb;
#ifndef CSIM
    // handlePbiRequest2()'s first request maps the screen (SAVMSC, 800
    // bytes). BENCH maps only $0000-$07FF as base memory, so the default
    // SAVMSC of 0 would need a copy-in no 6502 does: point it at $0400.
    atariRam[88] = 0x00;
    atariRam[89] = 0x04;
#endif
    dcb->DDEVIC = 0x31;
    dcb->DUNIT = (uint8_t)(op.drive + 1);
    dcb->DCOMND = op.kind == Kind::Status ? 0x53 : op.kind == Kind::Write ? 0x50 : 0x52;
    dcb->DSTATS = op.kind == Kind::Write ? 0x80 : 0x40;
    dcb->DBUFLO = (uint8_t)op.addr;
    dcb->DBUFHI = (uint8_t)(op.addr >> 8);
    dcb->DTIMLO = 7;
    dcb->DBYTLO = (uint8_t)op.len;
    dcb->DBYTHI = (uint8_t)(op.len >> 8);
    dcb->DAUX1 = (uint8_t)op.sector;
    dcb->DAUX2 = (uint8_t)(op.sector >> 8);
    PbiIocb *req = request();
    req->cmd = 7;   // SIO, see the DCB
    req->result = 0;
    req->carry = 0;
    req->req = op.flags;   // last: this is what the dispatch watches
}

// Pick and start the next request.
void startNext()
{
    op = {};
    op.drive = (int)(rng() % kDrives);
    if (drives[op.drive].absent) {
        op.drive = (op.drive + 1) % kDrives;
        if (drives[op.drive].absent) {
            return;   // nothing to test
        }
    }
    op.flags = REQ_FLAG_NORMAL;
    op.issuedSec = elapsedSec;
    Drive &d = drives[op.drive];
    if (d.sectorSize == 0) {
        op.kind = Kind::Status;
        op.sector = 0;
        op.len = 4;
    } else {
        const uint32_t r = rng() % 100;
        op.kind = r < 45 ? Kind::Write : Kind::Read;
        op.sector = 1 + (int)(rng() % kSectors);
        if (op.kind == Kind::Read && (rng() % 4) != 0) {
            // Mostly re-read sectors already written, to verify them.
            for (int tries = 0; tries < 8 && d.gen[op.sector] == 0; ++tries) {
                op.sector = 1 + (int)(rng() % kSectors);
            }
        }
        op.len = (uint16_t)sectorBytes(op.drive, op.sector);
    }
    op.addr = (uint16_t)(kBufLo + rng() % (uint32_t)(kBufHi - kBufLo - op.len));
    op.mapped = rangeMapped(op.addr, op.len);
    if (op.kind == Kind::Write) {
        op.gen = nextGen++;
        if (nextGen == 0) {
            nextGen = 1;
        }
        for (int i = 0; i < op.len; ++i) {
            *view((uint16_t)(op.addr + i)) = pattern(op.drive, op.sector, op.gen, i);
        }
        ++stats.writes;
    } else {
        // Junk where the data should land, so stale bytes can't pass.
        for (int i = 0; i < op.len; ++i) {
            *view((uint16_t)(op.addr + i)) = (uint8_t)(0xA5 ^ i);
        }
        if (op.kind == Kind::Read) {
            ++stats.reads;
        } else {
            ++stats.statuses;
        }
    }
    ++(op.mapped ? stats.mappedOps : stats.nativeOps);
    ++stats.submitted;
    submit();
    phase = Phase::Pending;
}

const char *kindName(Kind k)
{
    return k == Kind::Status ? "status" : k == Kind::Write ? "write" : "read";
}

Msg *opMsg(MsgType type)
{
    Msg *m = newMsg(type);
    if (m) {
        m->kind = (uint8_t)op.kind;
        m->drive = (uint8_t)op.drive;
        m->mapped = op.mapped;
        m->sector = (uint16_t)op.sector;
        m->addr = op.addr;
        m->len = op.len;
        m->gen = drives[op.drive].gen[op.sector];
    }
    return m;
}

void mismatch(int at)
{
    ++stats.mismatches;
    if (stats.mismatches > kMaxReports) {
        return;
    }
    if (Msg *m = opMsg(MsgType::Mismatch)) {
        m->at = (uint16_t)at;
        for (int i = 0; i < 8 && at + i < op.len; ++i) {
            m->got[i] = *view((uint16_t)(op.addr + at + i));
            m->expected[i] = pattern(op.drive, op.sector, m->gen, at + i);
        }
        postMsg();
    }
}

// The ESP's side of the PBI copy protocol, as far as a bench with no 6502
// can check it: an unmapped buffer's data must go through COPYBUF.
void protocolError(const char *what)
{
    ++stats.protocol;
    if (stats.protocol <= kMaxReports) {
        if (Msg *m = opMsg(MsgType::Protocol)) {
            const PbiIocb *req = request();
            m->what = what;
            m->a = req->result;
            m->copybuf = req->copybuf;
            m->copylen = req->copylen;
            postMsg();
        }
    }
}

// The request is done: check it the way the caller of SIO would.
void complete()
{
    const PbiIocb *req = request();
    const AtariDCB *dcb = atariMem.dcb;
    const bool ok = req->carry == 1 && dcb->DSTATS == 1;
    ++stats.completed;
    if (!ok && op.kind == Kind::Status) {
        // No such drive here (the bench without the Sidecar has only D1:).
        drives[op.drive].absent = true;
        if (Msg *m = opMsg(MsgType::Absent)) {
            m->a = req->carry;
            m->b = dcb->DSTATS;
            postMsg();
        }
        return;
    }
    if (!ok) {
        ++stats.failed;
        if (stats.failed <= kMaxReports) {
            if (Msg *m = opMsg(MsgType::Failed)) {
                m->a = req->carry;
                m->b = dcb->DSTATS;
                m->c = req->result;
                postMsg();
            }
        }
        return;
    }
    Drive &d = drives[op.drive];
    if (op.kind == Kind::Write && !op.mapped && !op.dataCopiedIn) {
        protocolError("an unmapped write completed without copying its data in");
    }
    switch (op.kind) {
    case Kind::Status:
        d.sectorSize = (*view(op.addr) & 0x20) ? 256 : 128;
        if (Msg *m = opMsg(MsgType::Status)) {
            m->a = *view(op.addr);
            m->len = (uint16_t)d.sectorSize;
            postMsg();
        }
        break;
    case Kind::Write:
        d.gen[op.sector] = op.gen;
        break;
    case Kind::Read:
        if (d.gen[op.sector] != 0) {
            for (int i = 0; i < op.len; ++i) {
                if (*view((uint16_t)(op.addr + i)) != pattern(op.drive, op.sector, d.gen[op.sector], i)) {
                    mismatch(i);
                    return;
                }
            }
            ++stats.verified;
        }
        break;
    }
}

void step()
{
    PbiIocb *req = request();
    if (req->req != 0) {
        if (elapsedSec - op.issuedSec > kTimeoutSec) {
            ++stats.timeouts;
            if (Msg *m = opMsg(MsgType::Timeout)) {
                m->a = req->req;
                postMsg();
            }
            req->req = 0;   // give up on it
            phase = Phase::Idle;
            lastDoneTicks = esp_cpu_get_cycle_count();
        }
        return;   // not served yet
    }
    const uint8_t result = req->result;
    // What the PBI ROM does with a result (pbirom.asm, RETRY_COMMAND):
    if (result & (RES_FLAG_NEED_DETACHSAFE | RES_FLAG_NEED_COPYIN)) {
        if (++op.retries > kMaxRetries) {
            ++stats.badRetries;
            if (Msg *m = opMsg(MsgType::GiveUp)) {
                m->a = (uint8_t)op.retries;
                m->b = result;
                postMsg();
            }
            phase = Phase::Idle;
            lastDoneTicks = esp_cpu_get_cycle_count();
            return;
        }
        if (result & RES_FLAG_NEED_DETACHSAFE) {
            op.flags |= REQ_FLAG_DETACHSAFE;
            ++stats.detachSafe;
        }
        if (result & RES_FLAG_NEED_COPYIN) {
            // COPYIN: copylen bytes from the 6502's memory at copybuf to COPYBUF.
            const uint16_t len = req->copylen > REQ_MAX_COPYLEN ? REQ_MAX_COPYLEN : req->copylen;
            for (uint16_t i = 0; i < len; ++i) {
                copyBuf()[i] = *view((uint16_t)(req->copybuf + i));
            }
            op.flags |= REQ_FLAG_COPYIN;
            ++stats.copyIns;
            if (req->copybuf == op.addr && req->copylen >= op.len) {
                op.dataCopiedIn = true;   // (other copy-ins: the screen, ...)
            }
        }
        op.issuedSec = elapsedSec;
        submit();
        return;
    }
    if (op.kind != Kind::Write && !op.mapped && req->carry == 1) {
        // An unmapped buffer: the data must come back through COPYBUF.
        if (!(result & RES_FLAG_COPYOUT) || req->copybuf != op.addr || req->copylen != op.len) {
            protocolError("an unmapped read completed without a copy-out of its buffer");
        } else if (op.kind == Kind::Read && drives[op.drive].gen[op.sector] != 0) {
            // Check COPYBUF itself, before the copy-out: what the ESP put there.
            for (int i = 0; i < op.len; ++i) {
                if (copyBuf()[i] != pattern(op.drive, op.sector, drives[op.drive].gen[op.sector], i)) {
                    protocolError("COPYBUF holds the wrong data");
                    break;
                }
            }
            ++stats.copyBufVerified;
        }
    }
    if (result & RES_FLAG_COPYOUT) {
        // COPYOUT: copylen bytes from COPYBUF to the 6502's memory at copybuf.
        const uint16_t len = req->copylen > REQ_MAX_COPYLEN ? REQ_MAX_COPYLEN : req->copylen;
        for (uint16_t i = 0; i < len; ++i) {
            *view((uint16_t)(req->copybuf + i)) = copyBuf()[i];
        }
        ++stats.copyOuts;
    }
    complete();
    phase = Phase::Idle;
    lastDoneTicks = esp_cpu_get_cycle_count();
}

void progress()
{
    printf("FAKE_SIO: %u requests (%u writes, %u reads: %u verified, %u mismatches), %u failed, %u timeouts, "
           "copy in/out %u/%u (COPYBUF checked %u), detach-safe %u, buffers mapped/native %u/%u, "
           "protocol errors %u\n",
           (unsigned)stats.completed, (unsigned)stats.writes, (unsigned)stats.reads,
           (unsigned)stats.verified, (unsigned)stats.mismatches, (unsigned)stats.failed,
           (unsigned)stats.timeouts, (unsigned)stats.copyIns, (unsigned)stats.copyOuts,
           (unsigned)stats.copyBufVerified, (unsigned)stats.detachSafe, (unsigned)stats.mappedOps,
           (unsigned)stats.nativeOps, (unsigned)stats.protocol);
}

void printMsg(const Msg &m)
{
    const char *kind = kindName((Kind)m.kind);
    const char *where = m.mapped ? "mapped" : "native";
    switch (m.type) {
    case MsgType::Status:
        printf("FAKE_SIO: D%d: status %02x: %u-byte sectors\n", m.drive + 1, m.a, m.len);
        break;
    case MsgType::Absent:
        printf("FAKE_SIO: D%d: no drive (status failed: carry %u DSTATS %02x); not testing it\n",
               m.drive + 1, m.a, m.b);
        break;
    case MsgType::Mismatch:
        printf("FAKE_SIO MISMATCH: D%d: sector %u gen %u at $%04x+%u (%s), first bad byte %u:", m.drive + 1,
               m.sector, m.gen, m.addr, m.len, where, m.at);
        for (int i = 0; i < 8 && m.at + i < m.len; ++i) {
            printf(" %02x/%02x", m.got[i], m.expected[i]);
        }
        printf(" (got/expected)\n");
        break;
    case MsgType::Failed:
        printf("FAKE_SIO FAILED: %s D%d: sector %u at $%04x+%u (%s): carry %u DSTATS %02x result %02x\n",
               kind, m.drive + 1, m.sector, m.addr, m.len, where, m.a, m.b, m.c);
        break;
    case MsgType::Protocol:
        printf("FAKE_SIO PROTOCOL: %s D%d: sector %u at $%04x+%u (%s): %s (result %02x, copybuf $%04x+%u)\n",
               kind, m.drive + 1, m.sector, m.addr, m.len, where, m.what, m.a, m.copybuf, m.copylen);
        break;
    case MsgType::Timeout:
        printf("FAKE_SIO TIMEOUT: %s D%d: sector %u, req %02x not served in %d s\n", kind, m.drive + 1,
               m.sector, m.a, kTimeoutSec);
        break;
    case MsgType::GiveUp:
        printf("FAKE_SIO: %s D%d: sector %u: %u re-issues, result %02x: giving up\n", kind, m.drive + 1,
               m.sector, m.a, m.b);
        break;
    }
}

}  // namespace

void fakeSioInit()
{
#ifdef CSIM
    native = const_cast<uint8_t *>(atariRam);   // atari800's RAM is the native RAM
#else
    native = static_cast<uint8_t *>(heap_caps_calloc(1, 0x10000, MALLOC_CAP_SPIRAM));
    if (!native) {
        native = static_cast<uint8_t *>(heap_caps_calloc(1, 0x10000, MALLOC_CAP_8BIT));
    }
#endif
    rngState = esp_random() | 1U;
    printf("FAKE_SIO: seed %08" PRIx32 ", buffers $%04x-$%04x, %d ms between requests%s\n", rngState, kBufLo,
           kBufHi, FAKE_SIO_GAP_MS, native ? "" : "; NO MEMORY for the native RAM shadow: not running");
}

void fakeSioFlush()
{
    while (msgTail != msgHead) {
        printMsg(msgs[msgTail % kMsgs]);
        ++msgTail;
    }
    if (msgsDropped) {
        printf("FAKE_SIO: %u messages dropped (queue full)\n", (unsigned)msgsDropped);
        msgsDropped = 0;
    }
    if (progressDue) {
        progressDue = false;
        progress();
    }
}

void fakeSioPoll()
{
    switch (phase) {
    case Phase::Start:
        if (elapsedSec < kStartSec || !native) {
            return;   // (no native shadow: fakeSioInit() said so; never runs)
        }
#ifdef CSIM
        // csim maps all 64K of atari800's RAM as base memory; the bench maps
        // only $0000-$07FF. Unmap a stretch of the buffer range (pages to
        // dummy RAM, nothing freed: it's atari800's), so buffers land
        // mapped, native and across the boundaries, and the copy protocol's
        // checks run.
        mmuUnmapRange(0x4000, 0x7fff);
#endif
        phase = Phase::Idle;
        lastDoneTicks = esp_cpu_get_cycle_count() - kGapTicks;
        lastProgressSec = elapsedSec;
        return;
    case Phase::Idle:
        if (esp_cpu_get_cycle_count() - lastDoneTicks >= kGapTicks) {
            startNext();
        }
        break;
    case Phase::Pending:
        if (native) {
            step();
        }
        break;
    }
    if (elapsedSec - lastProgressSec >= 30) {
        lastProgressSec = elapsedSec;
        progressDue = true;   // printed by fakeSioFlush()
    }
}

void fakeSioReport()
{
    fakeSioFlush();
    progress();
    const bool pass = stats.completed > 0 && stats.mismatches == 0 && stats.failed == 0 &&
                      stats.timeouts == 0 && stats.badRetries == 0 && stats.protocol == 0;
    printf("FAKE_SIO %s\n", pass ? "PASS" : "FAIL");
}

#endif  // FAKE_SIO
