// FAKE_SIO: see fakeSio.h.
#ifdef CSIM
#include "csim.h"
#endif
#include "fakeSio.h"

#ifdef FAKE_SIO
#include "asmDefs.h"
#include "extMem.h"
#include "main.h"
#include "mmu.h"
#include "pbi.h"

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_random.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Bank ops: $D301 writes that switch the extended-memory bank at $4000,
// so buffers land in the banks too, and canaries check each bank keeps its
// contents (across PSRAM evictions, on the bench).
//  - csim's bus-cycle mode: core 0 makes the bus cycles itself
//    (csim_fake_bus_cycle()).
//  - The FAKE_CLOCK bench: core 0 points the fake bus at $D300/$D301 by
//    reprogramming the address and data lines' pulls (and holding R/W high
//    while they settle), so core 1's own loop latches the PORTB writes.
//
// NOT FOR A REAL ATARI. The bench path writes IO_MUX and GPIO matrix
// registers from core 0 while core 1 serves the bus, which a hot bus can't
// survive: core 0's peripheral accesses stall core 1's GPIO accesses and
// wreck its cycle timing ([[no-peripherals-while-core1-hot]] in the notes).
// With the fake clock there is no 6502 to crash, which is the only reason
// it's allowed here. FAKE_SIO as a whole is bench/csim only: it also plays
// the 6502's part in the PBI request block.
#if defined(CSIM_BUS_CYCLE)
#define FAKE_SIO_BANKS 1
int csim_fake_bus_cycle(uint16_t addr, bool read, uint8_t *data);   // csim_stubs.cpp
#elif defined(FAKE_CLOCK) && !defined(CSIM)
#define FAKE_SIO_BANKS 1
#define FAKE_SIO_BENCH_BANKS 1
#include "fakeBus.h"
#include "pinDefs.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/soc.h"
#elif !defined(CSIM)
#error "FAKE_SIO needs FAKE_CLOCK on hardware: it can't run against a real 6502"
#endif

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
#ifndef CSIM
constexpr uint16_t kMappedLo = 0x2000, kMappedHi = 0x3fff;   // mapped for the test (fakeSioInit())
#endif

#ifdef FAKE_SIO_BANKS
// Each bank's canary: kCanaryLen bytes at kCanaryAt, written through the bus
// (core 1's write path into the bank) after every switch to it, and read
// back through the bus at the next. The SIO buffers stay clear of it.
constexpr uint16_t kCanaryAt = 0x7ff0;
constexpr int kCanaryLen = 16;
#endif

constexpr int kDrives = 2;           // D1: (the Sidecar's, with the link), D2: (local)
constexpr int kSectors = 720;        // both test images: 720 sectors
constexpr int kMaxReports = 8;       // mismatches printed in full

// Bank op phases: BankSettle, BankWrite and BankRestore only on the bench
// (the fake bus's pulls, below); csim goes straight to BankCheck.
enum class Phase { Start, Idle, Pending, BankSettle, BankWrite, BankRestore, BankCheck };
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
    bool preCopied;      // a write sent with its data already in COPYBUF (below)
    bool copyBufHolds;   // preCopied, and COPYBUF still holds its data
};

struct Stats {
    uint32_t submitted, completed, failed, timeouts;
    uint32_t writes, reads, verified, mismatches, statuses;
    uint32_t copyIns, copyOuts, detachSafe, mappedOps, nativeOps;
    uint32_t badRetries;
    uint32_t protocol;   // the ESP's side of the copy protocol misbehaved
    uint32_t copyBufVerified;   // reads checked in COPYBUF before the copy-out
    uint32_t preCopied;         // writes sent pre-copied, DETACHSAFE|COPYIN up front
    uint32_t bankSwitches, bankBase, canariesChecked, bankErrors;
    uint32_t busMismatches;   // of mismatches: read through core 1 (bank ops' builds)
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
enum class MsgType : uint8_t { Status, Absent, Mismatch, Failed, Protocol, Timeout, GiveUp, Bank };
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
// One core (core 0) adds and prints, never at once: no atomics needed.
uint32_t msgHead = 0, msgTail = 0, msgsDropped = 0;
bool progressDue = false;

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
    if (op.preCopied && op.retries == 0) {
        req->copybuf = op.addr;
        req->copylen = op.len;
    }
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
#ifdef FAKE_SIO_BANKS
    while (op.addr + op.len > kCanaryAt && op.addr < kCanaryAt + kCanaryLen) {
        op.addr = (uint16_t)(kBufLo + rng() % (uint32_t)(kBufHi - kBufLo - op.len));
    }
#endif
    op.mapped = rangeMapped(op.addr, op.len);
    if (op.kind == Kind::Write) {
        op.gen = nextGen++;
        if (nextGen == 0) {
            nextGen = 1;
        }
        // A third of the unmapped writes go out pre-copied, as a ROM that
        // knew the buffer is unmapped would send them: the data in COPYBUF,
        // copybuf/copylen naming the buffer, DETACHSAFE|COPYIN set from the
        // start. The buffer itself holds junk, so a later read of the sector
        // passes only if the ESP took the data from COPYBUF. (The ESP never
        // asks for DETACHSAFE today: needSafeWait() is unused, pbi.cpp.)
        op.preCopied = !op.mapped && rng() % 3 == 0;
        for (int i = 0; i < op.len; ++i) {
            const uint8_t b = pattern(op.drive, op.sector, op.gen, i);
            *view((uint16_t)(op.addr + i)) = op.preCopied ? (uint8_t)(b ^ 0x5a) : b;
            if (op.preCopied) {
                copyBuf()[i] = b;
            }
        }
        if (op.preCopied) {
            op.flags |= REQ_FLAG_DETACHSAFE | REQ_FLAG_COPYIN;
            op.dataCopiedIn = true;
            op.copyBufHolds = true;
            ++stats.preCopied;
            ++stats.detachSafe;
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

#ifdef FAKE_SIO_BANKS
uint16_t canaryGen[32];   // by extMem bank (premap[] value); 0: none yet
uint8_t bankPortb;        // the PORTB value the last bank op wrote
uint16_t bankGenNext = 1;

uint8_t canary(int bank, uint16_t gen, int i) { return pattern(2 + bank, 0, gen, i); }

int bankOf(uint8_t portb) { return extMem.premap[(portb & 0x7c) >> 2]; }

void bankError(const char *what, uint16_t at = 0, uint8_t got = 0, uint8_t expected = 0)
{
    ++stats.bankErrors;
    if (stats.bankErrors <= kMaxReports) {
        if (Msg *m = newMsg(MsgType::Bank)) {
            m->what = what;
            m->a = bankPortb;
            m->b = (uint8_t)bankOf(bankPortb);
            m->at = at;
            m->got[0] = got;
            m->expected[0] = expected;
            postMsg();
        }
    }
}

// The canaries' reads and writes: csim's go through the bus (core 1's loop
// body, as the 6502's would); the bench has no way to read the bus, so
// they go through the map as core 0 sees it, which is the table core 1
// selects from (extBanks[] by PORTB).
bool canaryRead(uint16_t a, uint8_t *v)
{
#ifdef CSIM_BUS_CYCLE
    return csim_fake_bus_cycle(a, true, v);
#else
    *v = *view(a);
    return rangeMapped(a, 1);
#endif
}

void canaryWrite(uint16_t a, uint8_t v)
{
#ifdef CSIM_BUS_CYCLE
    csim_fake_bus_cycle(a, false, &v);
#else
    *view(a) = v;
#endif
}

#ifdef FAKE_SIO_BENCH_BANKS
// The fake bus (fakeBus.cpp): the clock, A0 and R/W come from LEDC, and
// A1-A15 from each pin's pull, set for fakeBusTestAddress. The data lines
// float unless core 1 drives them. A bank op moves the pulls to $D300 and
// the data lines' pulls to the PORTB value, so the fake bus's write cycles
// write $D300 and $D301 (A0 toggles) with it.
//
// R/W is held high (reads only) while the pulls change, so the half-moved
// addresses on the way don't become writes: R/W's GPIO matrix output goes
// from its LEDC channel to the plain GPIO output, set high. Restoring the
// saved routing reconnects the LEDC channel, which kept counting, so the
// phase that fakeBusSignalsStart() set up is kept.
//
// AGAIN: core 0 writing these registers wrecks core 1's timing on a hot bus.
// Bench only (see the top of the file).
constexpr int kDataPins = 8;
constexpr int kAddrPins = 16;
uint32_t savedMux[kAddrPins + kDataPins];   // IO_MUX words, as setup left them
uint32_t savedRwOutSel;                     // R/W's GPIO matrix routing (LEDC)
bool benchPinsSaved = false;

volatile uint32_t *ioMux(int gpio)
{
    static_assert(IO_MUX_GPIO1_REG - IO_MUX_GPIO0_REG == 4, "IO_MUX_GPIOn_REG is linear");
    return reinterpret_cast<volatile uint32_t *>(IO_MUX_GPIO0_REG + 4 * gpio);
}

volatile uint32_t *outSel(int gpio)
{
    return reinterpret_cast<volatile uint32_t *>(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * gpio);
}

void benchPinsSave()
{
    for (int i = 0; i < kAddrPins; ++i) {
        savedMux[i] = *ioMux(bus.addr.pin + i);
    }
    for (int i = 0; i < kDataPins; ++i) {
        savedMux[kAddrPins + i] = *ioMux(bus.data.pin + i);
    }
    savedRwOutSel = *outSel(bus.rw.pin);
    benchPinsSaved = true;
}

void setPull(int gpio, bool high)
{
    *ioMux(gpio) = (*ioMux(gpio) & ~(FUN_PU | FUN_PD)) | (high ? FUN_PU : FUN_PD);
}

void holdRwHigh()
{
    static_assert(bus.rw.pin < 32, "R/W is in GPIO_OUT_REG");
    REG_WRITE(GPIO_OUT_W1TS_REG, 1U << bus.rw.pin);
    *outSel(bus.rw.pin) = SIG_GPIO_OUT_IDX | GPIO_FUNC0_OEN_SEL;   // GPIO_OUT, GPIO_ENABLE's OE
}

void releaseRw() { *outSel(bus.rw.pin) = savedRwOutSel; }

// The pulls for address addr (A1-A15; A0 stays on LEDC) and, if data >= 0,
// the data lines; data < 0 puts the data lines' IO_MUX words back.
void pointFakeBus(uint16_t addr, int data)
{
    for (int i = 1; i < kAddrPins; ++i) {
        setPull(bus.addr.pin + i, (addr >> i) & 1);
    }
    for (int i = 0; i < kDataPins; ++i) {
        if (data >= 0) {
            setPull(bus.data.pin + i, (data >> i) & 1);
        } else {
            *ioMux(bus.data.pin + i) = savedMux[kAddrPins + i];
        }
    }
}

constexpr uint32_t kSettleTicks = 50 * 240;        // 50 us: weak pulls into the pins' capacitance
constexpr uint32_t kWriteTicks = 2 * kTicksPerMs;  // ~1400 fake bus writes of $D301
#endif

// Switch banks: write PORTB, keeping its OS, BASIC and self-test bits (0,
// 1, 7), with random bank select bits (2-6: about half pick a board bank,
// half base memory). Core 0's bmon pass then pages the bank in (on the
// bench, evicting another to PSRAM) and lifts the halt; BankCheck looks at
// the result.
void startBankOp()
{
    bankPortb = (uint8_t)((d000Write[0x301] & 0x83) | ((rng() & 0x1f) << 2));
    ++stats.bankSwitches;
    lastDoneTicks = esp_cpu_get_cycle_count();
#ifdef FAKE_SIO_BENCH_BANKS
    if (!benchPinsSaved) {
        benchPinsSave();
    }
    holdRwHigh();
    pointFakeBus(0xd300, bankPortb);
    phase = Phase::BankSettle;
#else
    uint8_t v = bankPortb;
    csim_fake_bus_cycle(0xd301, false, &v);
    phase = Phase::BankCheck;
#endif
}

#ifdef FAKE_SIO_BENCH_BANKS
// The bench's bank op after startBankOp(), one step per poll.
void benchBankStep()
{
    const uint32_t since = esp_cpu_get_cycle_count() - lastDoneTicks;
    switch (phase) {
    case Phase::BankSettle:   // pulls at $D300/value: let the writes through
        if (since >= kSettleTicks) {
            releaseRw();
            phase = Phase::BankWrite;
            lastDoneTicks = esp_cpu_get_cycle_count();
        }
        break;
    case Phase::BankWrite:    // core 1 latching $D301, core 0 paging the bank in
        if (since >= kWriteTicks) {
            holdRwHigh();
            pointFakeBus(fakeBusTestAddress, -1);
            phase = Phase::BankRestore;
            lastDoneTicks = esp_cpu_get_cycle_count();
        }
        break;
    case Phase::BankRestore:  // pulls back at the test address
        if (since >= kSettleTicks) {
            releaseRw();
            phase = Phase::BankCheck;
            lastDoneTicks = esp_cpu_get_cycle_count();
        }
        break;
    default:
        break;
    }
}
#endif

void bankCheck()
{
    if (d000Write[0x301] != bankPortb) {
        bankError("PORTB didn't latch the write", 0x301, d000Write[0x301], bankPortb);
    }
    const int bank = bankOf(bankPortb);
    // $4000-$7FFF's base memory is unmapped (csim: fakeSioPoll()'s Start;
    // the bench maps only $0000-$07FF and $2000-$3FFF): mapped exactly when
    // a board bank is selected.
    if (rangeMapped(0x4000, 0x4000) != (bank >= 0)) {
        bankError(bank >= 0 ? "a board bank isn't mapped at $4000" : "base memory at $4000 is mapped");
    }
    if (bank < 0) {
        ++stats.bankBase;
    } else {
        for (int i = 0; canaryGen[bank] != 0 && i < kCanaryLen; ++i) {
            uint8_t got = 0;
            const uint8_t expected = canary(bank, canaryGen[bank], i);
            if (!canaryRead((uint16_t)(kCanaryAt + i), &got)) {
                bankError("the bank didn't answer a canary read", (uint16_t)(kCanaryAt + i));
                break;
            }
            if (got != expected) {
                bankError("canary changed", (uint16_t)(kCanaryAt + i), got, expected);
                break;
            }
            if (i == kCanaryLen - 1) {
                ++stats.canariesChecked;
            }
        }
        const uint16_t gen = bankGenNext++;
        if (bankGenNext == 0) {
            bankGenNext = 1;
        }
        for (int i = 0; i < kCanaryLen; ++i) {
            canaryWrite((uint16_t)(kCanaryAt + i), canary(bank, gen, i));
        }
        // The writes must land in the bank's own memory.
        for (int i = 0; i < kCanaryLen; ++i) {
            const uint8_t got = extMem.banks[bank][(kCanaryAt - 0x4000) + i];
            if (got != canary(bank, gen, i)) {
                bankError("a canary write missed the bank", (uint16_t)(kCanaryAt + i), got, canary(bank, gen, i));
                break;
            }
        }
        canaryGen[bank] = gen;
    }
    phase = Phase::Idle;
    lastDoneTicks = esp_cpu_get_cycle_count();
}
#endif

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

// A read's bad byte at offset at, as view() sees it or (viaBus) as a bus
// read through core 1 returns it.
void mismatch(int at, bool viaBus = false)
{
    ++stats.mismatches;
    if (stats.mismatches > kMaxReports) {
        return;
    }
    if (Msg *m = opMsg(MsgType::Mismatch)) {
        m->at = (uint16_t)at;
        for (int i = 0; i < 8 && at + i < op.len; ++i) {
            m->got[i] = *view((uint16_t)(op.addr + at + i));
#ifdef CSIM_BUS_CYCLE
            if (viaBus) {
                csim_fake_bus_cycle((uint16_t)(op.addr + at + i), true, &m->got[i]);
            }
#endif
            m->expected[i] = pattern(op.drive, op.sector, m->gen, at + i);
        }
        m->what = viaBus ? " through core 1" : "";
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
#ifdef CSIM_BUS_CYCLE
            // view() reads the map as core 0 sees it; read a mapped buffer
            // through core 1 too, as the 6502 would (a wrong bank select
            // there passes the check above).
            for (int i = 0; op.mapped && i < op.len; ++i) {
                uint8_t got = 0;
                if (!csim_fake_bus_cycle((uint16_t)(op.addr + i), true, &got) ||
                    got != pattern(op.drive, op.sector, d.gen[op.sector], i)) {
                    ++stats.busMismatches;
                    mismatch(i, true);
                    return;
                }
            }
#endif
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
            const bool ours = op.preCopied && req->copybuf == op.addr;
            if (ours && op.copyBufHolds) {
                protocolError("asked to copy in a buffer already in COPYBUF");
            }
            for (uint16_t i = 0; i < len; ++i) {
                // A pre-copied write's buffer holds junk: its data is the
                // pattern (asked for again after another copy-in, e.g. the
                // screen's, displaced it).
                copyBuf()[i] = ours && i < op.len ? pattern(op.drive, op.sector, op.gen, i)
                                                  : *view((uint16_t)(req->copybuf + i));
            }
            if (op.preCopied) {
                op.copyBufHolds = ours;
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
           "copy in/out %u/%u (COPYBUF checked %u, pre-copied writes %u), detach-safe %u, buffers mapped/native %u/%u, "
           "protocol errors %u, banks %u (%u base, %u canaries checked, %u errors, %d evictions), messages %u/%u\n",
           (unsigned)stats.completed, (unsigned)stats.writes, (unsigned)stats.reads,
           (unsigned)stats.verified, (unsigned)stats.mismatches, (unsigned)stats.failed,
           (unsigned)stats.timeouts, (unsigned)stats.copyIns, (unsigned)stats.copyOuts,
           (unsigned)stats.copyBufVerified, (unsigned)stats.preCopied, (unsigned)stats.detachSafe, (unsigned)stats.mappedOps,
           (unsigned)stats.nativeOps, (unsigned)stats.protocol, (unsigned)stats.bankSwitches,
           (unsigned)stats.bankBase, (unsigned)stats.canariesChecked, (unsigned)stats.bankErrors, extMem.evictCount,
           (unsigned)msgTail, (unsigned)msgHead);
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
        printf("FAKE_SIO MISMATCH%s: D%d: sector %u gen %u at $%04x+%u (%s), first bad byte %u:", m.what,
               m.drive + 1, m.sector, m.gen, m.addr, m.len, where, m.at);
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
    case MsgType::Bank:
        printf("FAKE_SIO BANK: PORTB %02x (bank %d): %s", m.a, (int8_t)m.b, m.what);
        if (m.at) {
            printf(" at $%04x: %02x, expected %02x", m.at, m.got[0], m.expected[0]);
        }
        printf("\n");
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
    // BENCH maps only $0000-$07FF, below the buffers: map $2000-$3FFF as
    // base RAM too (8K of SRAM, setup time), so buffers land mapped, native
    // and across both boundaries, as in csim.
    mmuAllocAddBaseRam(kMappedLo, kMappedHi);
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
#ifdef FAKE_SIO_BANKS
            if (extMem.banks && rng() % 6 == 0) {
                startBankOp();
                break;
            }
#endif
            startNext();
        }
        break;
#ifdef FAKE_SIO_BANKS
    case Phase::BankSettle:
    case Phase::BankWrite:
    case Phase::BankRestore:
#ifdef FAKE_SIO_BENCH_BANKS
        benchBankStep();
#endif
        break;
    case Phase::BankCheck:
        // A millisecond for core 0's bmon pass (page in, resume the 6502).
        if (esp_cpu_get_cycle_count() - lastDoneTicks >= kTicksPerMs) {
            bankCheck();
        }
        break;
#else
    case Phase::BankSettle:
    case Phase::BankWrite:
    case Phase::BankRestore:
    case Phase::BankCheck:
        break;
#endif
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
    for (int d = 0; d < kDrives; ++d) {
        if (drives[d].absent) {
            printf("FAKE_SIO: D%d: absent\n", d + 1);
        } else {
            printf("FAKE_SIO: D%d: %d-byte sectors\n", d + 1, drives[d].sectorSize);
        }
    }
    progress();
    const bool pass = stats.completed > 0 && stats.mismatches == 0 && stats.failed == 0 &&
                      stats.timeouts == 0 && stats.badRetries == 0 && stats.protocol == 0 &&
                      stats.bankErrors == 0;
    printf("FAKE_SIO %s\n", pass ? "PASS" : "FAIL");
}

#endif  // FAKE_SIO
