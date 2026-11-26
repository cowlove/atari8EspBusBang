#ifdef CSIM
#include "csim.h"
#endif
#ifndef CSIM
#include "soc/usb_serial_jtag_reg.h"   // refresh_ on GPIO19: USB pad off
#endif

#pragma GCC optimize("Ofast")
#include "a8log.h"
#include "a8log_host.h"
#include <esp_intr_alloc.h>
#include <rtc_wdt.h>
#include <esp_task_wdt.h>
#include <esp_task.h>
#include <esp_async_memcpy.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_reg.h"
#include <rtc_wdt.h>
#include <hal/cache_hal.h>
#include <hal/cache_ll.h>
#include <xtensa/core-macros.h>
#include <xtensa/hal.h>
#include <xtensa_timer.h>
#include <xtensa_rtos.h>
#include "rom/ets_sys.h"
#include "soc/dport_access.h"
#include "soc/system_reg.h"
#ifndef CSIM
#include "esp_cpu.h"   // esp_cpu_get_core_id(), for __wrap_esp_panic_handler
#ifndef CSIM
#include "soc/assist_debug_reg.h"         // core 1's PC record, for a hang
#include "xtensa/xtruntime.h"              // _xtos_set_exception_handler (ROM): core 1's faults
#endif
#include "heap_memory_layout.h"   // SOC_RESERVE_MEMORY_REGION
#include "esp_heap_caps_init.h"   // heap_caps_add_region
#include "esp_system.h"   // esp_reset_reason(), for ROM_BOOT_PROBE=2
#include "esp_memory_utils.h"   // esp_ptr_internal(), for structLogs
#endif
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_mac.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#ifndef CSIM
// registered_heaps (logHeapInfo()). heap_t's layout as the heap component
// compiles it: its CMakeLists passes -DMULTI_HEAP_FREERTOS (the lock type).
#define MULTI_HEAP_FREERTOS
#include "heap_private.h"
#endif
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include <deque>
#include <functional>
#include <algorithm>
#include <inttypes.h>
#include <strings.h>
#include "soc/soc_caps.h"

#ifdef ATARI800_SIM
extern "C" uint8_t MEMORY_mem[65538];
#endif


#if !defined(CSIM) && CONFIG_FREERTOS_UNICORE != 1
#error Arduino idf core must be compiled with CONFIG_FREERTOS_UNICORE=y and CONFIG_ESP_INT_WDT=n
#endif

#include <new>   // placement new, for structLogs
#include <vector>
#include <string>
using std::vector;
using std::string;
#include "ascii2keypress.h"
#include "pinDefs.h"
#include "main.h"
#include "fakeBus.h"
#ifndef ARDUINO
#include "arduinoLite.h"
#endif

#include "asmDefs.h"
#include "extMem.h"
#include "util.h" 
#include "screenshot.h"
#include <time.h>   // libsmb2's headers use time_t without including it
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#include "hostSio.h"
#include "telnetScreen.h"
#include "diskFlash.h"
#include "diskStitch.h"
#include "procFsNodes.h"
#include "cartridge.h"
#include "sysMonitor.h"
#include "sysConfig.h"
#include <fcntl.h>   // SysConfig::save()
#include <unistd.h>
#include "led.h"
#include "pbi.h"
#include "sfmt.h"
#include "const.h"
#include "bmon.h"
#include "mmu.h"
#include "profile.h"
#include "led.h"
#include "log.h"
#include "cio.h"
#include "hwType.h"
#include "sidecarHost.h"
#include "sidecar.h"   // SidecarSampleAction, SidecarSystemFlag (also without the Sidecar)
#include "fakeSio.h"
#include "hotData.h"


void sidecarSampling(SidecarSampleAction action, uint8_t category);   // below
void sidecarSystemFlags(uint32_t set, uint32_t clear);               // below
static bool noDrivesConfigured() {
    for (const auto &d : config.diskSpec) if (!d.empty()) return false;
    return true;
}
void raiseInterrupt();                                               // below
extern volatile int rebootRequested;      // pbi.cpp
#ifdef CSIM
void csim_main_reboot_request();          // csim_atari_reset.cpp: main's reboot, as its EN
#endif

// A safe reboot of main under a running Atari. From the Sidecar's SetFlags
// Reboot: the next IRQ call into the PBI handler (raiseInterrupt()) goes
// cold, as every IRQ call does (the detach-safe reissue), and there, with
// the bus detached and the 6502 in the stack-resident wait in its own page
// 1 (SAFE_WAIT, pbirom.asm: interrupts masked, NMIs off), main reboots
// (safeRebootParked()) instead of returning. Main's next boot catches the
// 6502 in that loop (the sled) and cold-starts it.
IRAM_ATTR void requestSafeReboot() {
    rebootRequested = 1;
    raiseInterrupt();
}
static void rebootRestart() {
#ifdef CSIM
    csim_main_reboot_request();
#else
    esp_restart();
#endif
}
// handlePbiRequestCold()'s IRQ branch, the bus detached: ask the Sidecar
// for the reset (RebootReady), wait for it, and restart main if it doesn't
// come in 3 s (standalone: at once). Never returns.
void safeRebootParked() {
    rebootRequested = 0;
#if A8BB_SIDECAR
    if (sidecarHost.sidecar().present() && !sidecarHost.sidecar().loopback()) {
        LOG("REBOOT: the 6502 parked in its own RAM; the Sidecar resets main\n");
        sidecarSystemFlags(static_cast<uint32_t>(SidecarSystemFlag::RebootReady), 0);
        for (const uint32_t t0 = XTHAL_GET_CCOUNT(); XTHAL_GET_CCOUNT() - t0 < 240u * 1000 * 3000;) {
            CSIM_HOOK();
        }
        LOG("REBOOT: no reset from the Sidecar in 3 s; main restarts\n");
    } else
#endif
    LOG("REBOOT: the 6502 parked in its own RAM; main restarts\n");
    rebootRestart();
#ifdef CSIM
    for (;;) { CSIM_HOOK(); (void)XTHAL_GET_CCOUNT(); }   // the reboot happens at the next CPU tick
#endif
}

DRAM_ATTR AtariMemPointers atariMem;


// boot SDX cartridge image - not working well enough to base stress tests on it 
//#define BOOT_SDX

#ifndef BOOT_SDX
#define RAMBO_XL256
#endif

#if 0 // TMP: investigate removing these, should be unneccessary due to linker script
#undef DRAM_ATTR
#define DRAM_ATTR
#undef IRAM_ATTR
#define IRAM_ATTR 
#endif 

//DRAM_ATTR BUSCTL_VOLATILE uint32_t pinEnableMask = ~0;

//DRAM_ATTR uint32_t busEnabledMark;
//DRAM_ATTR BUSCTL_VOLATILE uint32_t pinDriveMask = 0;
//DRAM_ATTR int busWriteDisable = 0;

DRAM_ATTR int ioCount = 0, pbiInterruptCount = 0, memWriteErrors = 0, unmapCount = 0, 
    watchDogCount = 0, spuriousHaltCount = 0, haltCount = 0;
// resume6502()'s 20 us limit reached before core 1 recorded 2 reads (the
// halt release was blind: the resume-halt race's window). Normally 0.
DRAM_ATTR unsigned resumeTimeouts = 0;
#ifdef HAVE_REFRESH
// Writes that saw refresh_ low. A write is never a refresh cycle, so any
// means refresh_ is stuck low or miswired, and core 1 then records no reads
// at all (it skips refresh cycles): resume6502() times out on every halt,
// and core 1's watchdog sees only writes. Warned once (core0Loop()).
DRAM_ATTR unsigned writesRefreshLow = 0;
DRAM_ATTR static bool refreshLowWarned = false;
#endif
DRAM_ATTR string exitReason = "";
DRAM_ATTR int elapsedSec = 0;
DRAM_ATTR int exitFlag = 0;

// noinline (IFLASH_ATTR) and cold keep the exit code out of the hot-bus
// callers, so they only make one call to it.
IFLASH_ATTR __attribute__((cold)) void core0_exit(int flag, const char *format, ...) {
    va_list args;
    va_start(args, format);
    exitReason = vsfmt(format, args);
    va_end(args);
    exitFlag = flag;
}
DRAM_ATTR uint32_t lastVblankTsc = 0;

DRAM_ATTR Sio sio;

void mountDiskSpecs(const std::string (&diskSpecs)[Sio::kDrives])
{
    static constexpr std::string_view kSidecarPrefix = "sidecar!";
    for (int d = 0; d < Sio::kDrives; d++) {
        const std::string &ds = diskSpecs[d];
        if (!ds.starts_with(kSidecarPrefix)) {
#if !CONFIG_A8BB_WIFI
            // No network stack without WiFi (A8BB_WIFI): libsmb2 crashed main
            // at boot (.6, SDX_AUTO's D4). Leave the drive empty.
            if (ds.starts_with("smb:")) {
                sio.unmount(d);
                LOG("D%d: '%s': no WiFi in this build (A8BB_WIFI); not mounted\n", d + 1, ds.c_str());
                continue;
            }
#endif
            sio.mount(d, ds);
            continue;
        }
        sio.unmount(d);   // the drive is the Sidecar's; nothing local
        const std::string sidecarSpec = ds.substr(kSidecarPrefix.size());
#if A8BB_SIDECAR
        sidecarHost.mount(d + 1, sidecarSpec);
#else
        LOG("D%d: '%s': no Sidecar in this build (A8BB_SIDECAR)\n", d + 1,
               sidecarSpec.c_str());
#endif
    }
}

DRAM_ATTR bool diskRotatePending = false;

CORE0_HOTSPOTS_ATTR void rotateDrives()
{
#if A8BB_SIDECAR && !A8BB_SIDECAR_LOOPBACK
    if (!sidecarHost.rotateDrives()) {
        LOGHOT("disk swap: the Sidecar didn't rotate its drives; none rotated\n");
        return;
    }
#endif
    sio.rotate();
    LOGHOT("disk swap: drives rotated (D1: has D2:'s disk ... D8: D1:'s)\n");
}

DRAM_ATTR LedRmt led;  

DRAM_ATTR int deferredInterrupt = 0, interruptRequested = 0, sysMonitorRequested = 0;
DRAM_ATTR volatile bool irqJammed = false;   // core 0 holds irq_ itself (irqJam(), below)

IRAM_ATTR void raiseInterrupt() {
    CORE0_PHASE(PH_IRQ, 0);
    if ((d000Write[_0x1ff] & pbiDeviceNumMask) != pbiDeviceNumMask
        && (d000Write[_0x301] & 0x1) != 0
    ) {
        deferredInterrupt = 0;  
        d000Read[_0x1ff] = pbiDeviceNumMask;
        atariRam[PDIMSK] |= pbiDeviceNumMask;
        mmuState.pinReleaseMask &= bus.irq_.maskInverse;
        for(auto &b : l1Blocks)
            for (auto &ctrl : b.ctrl) 
                ctrl |= bus.irq_.mask;
        interruptRequested = 1;
    } else { 
        deferredInterrupt = 1;
    }
}

void clearInterrupt() { 
    for(auto &b : l1Blocks)
        for (auto &ctrl : b.ctrl) 
            ctrl &= bus.irq_.maskInverse;
    busyWait6502Ticks(2);
    if (!irqJammed) mmuState.pinReleaseMask |= bus.irq_.mask;   // the jam keeps the line
    interruptRequested = 0;
    busyWait6502Ticks(10);
    d000Read[_0x1ff] = 0x0;
    atariRam[PDIMSK] &= pbiDeviceNumMaskNOT;
}

// The IRQ jam: core 0 holding irq_ low itself, not through core 1's page
// tables, from the start of setup(), through the cold-bus setup and core
// 1's start, until irqUnjam(). For bringing up a machine with no working
// halt (a 130XE) whose 6502 is already running: an IRQ held low keeps it
// looping in the OS's IRQ handler (when its I flag is clear) instead of
// running its program while main changes the memory under it.
// pinReleaseMask without irq_ keeps core 1's per-cycle release (W1TC) off
// the pin once it starts; clearInterrupt() leaves it out while jammed.
// csim: the pin state its bus cycles track gets the write too
// (csim_core0_gpio_enable1(), csim_atari800.cpp).
#ifdef CSIM
void csim_core0_gpio_enable1(uint32_t set, uint32_t clear);
#endif
// Cold only: core 1 not yet running the bus (setup()).
void irqJam() {
    digitalWrite(bus.irq_.pin, 0);
    pinMode(bus.irq_.pin, OUTPUT_OPEN_DRAIN);
    mmuState.pinReleaseMask &= bus.irq_.maskInverse;
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, bus.irq_.mask);
#ifdef CSIM
    csim_core0_gpio_enable1(bus.irq_.mask, 0);
#endif
    irqJammed = true;
}
// Lift it. hot (core 1 running the bus): only pinReleaseMask, so core 1's
// next cycle releases the pin (no peripheral access from core 0 then);
// cold: the register too. A pending raiseInterrupt() keeps the line.
IRAM_ATTR void irqUnjam(bool hot) {
    irqJammed = false;
    if (interruptRequested) return;
    mmuState.pinReleaseMask |= bus.irq_.mask;
    if (!hot) {
        REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.irq_.mask);
#ifdef CSIM
        csim_core0_gpio_enable1(0, bus.irq_.mask);
#endif
    }
}

#if defined(SLED_TEST) && SLED_TEST
// The sled landing (AGENTS.md, power-on with no working halt; prototype,
// -DSLED_TEST=1): catching a 6502 that was already running when main came
// up, and cold-starting it. The "$4C trick": core 1 starts on the
// transparent map (mmuStateDisabled: it drives nothing, native memory
// answers) and warms up; then $0000-$7FFF (every ext bank too) goes live,
// pointer by pointer (every state between is safe), reading $4C
// everywhere, writes going nowhere. Any instruction fetched there, at any
// alignment, is JMP $4C4C, and $4C4C loops on itself; RTS pops $4C4C
// (+1: still JMP $4C4C), RTI pops PC $4C4C (and P $4C: I set, D and V
// too: the stub's CLD); every vector the OS's interrupt handlers jump
// through is $4C4C. Pushes go nowhere, so the first RTS or RTI lands even
// from ROM, as does the next VBI or IRQ; no IRQ jam. $8000 up stays
// transparent: what was there before main came, and no halt bits (a
// halting $D3xx write would wait on core0Loop(), which the wait below
// doesn't run). sledLanding() (threadFunc(), the bus hot) sees the loop
// in the read ring ($4C4C-$4C4E and nothing else), turns its JMP to the
// stub in page 6 (low byte, then high: $4C4C, $4C00 (flood: back to
// $4C4C), $0600), waits for the stub's mark, makes the reset a cold start
// (PUPBT1, COLDST), puts the real map back (enableBus(): page 6, main's
// base RAM, is the same memory in both maps) and lets the stub go: NMIs
// off, PORTB $FF, JMP ($FFFC).
static constexpr uint16_t kSledStub = 0x0600, kSledGo = 0x06fe, kSledLanded = 0x06ff, kSledLoop = 0x4c4c;
static constexpr uint8_t kSledLandedMark = 0xa5, kSledFlood = 0x4c;
static DRAM_ATTR uint8_t sledPage[pageSize];
// Page 0 is a scratch page of main's, read and written (native RAM still
// blocked), first flooded too: a loop counting in zero page then ends as
// it would (on the read-only flood it reads $4C forever), so code that
// never returns still reaches its CLI or NMI enable and lands then; stack
// pulls (page 1) and vectors (page 2) stay on the read-only flood. A
// precaution: AltirraOS's reset-time init lands without it (csim,
// 2026-11-26); the XL OS's init is untested.
static DRAM_ATTR uint8_t sledZeroPage[pageSize];
static L1Block *sledLow, *sledAll;   // block 0 (pages 0 and 6 real) and the rest

static void sledFill(L1Block *b, bool low) {
    for (int p = 0; p < pagesPerL1Block; p++)
        for (int vid : PAGESEL_EXTRA_VARIATIONS) {
            const bool stub = low && p == pageNr(kSledStub);
            const bool zero = low && p == 0;   // block 0's page 0: the scratch page
            b->pages[p | PAGESEL_RD | vid] = stub ? atariRam + kSledStub : zero ? sledZeroPage : sledPage;
            b->pages[p | PAGESEL_WR | vid] = stub ? atariRam + kSledStub : zero ? sledZeroPage : (uint8_t *)dummyRam;
            b->ctrl[p | PAGESEL_RD | vid] = bus.data.mask | bus.ctrlExtSel.mask;
            b->ctrl[p | PAGESEL_WR | vid] = bus.ctrlExtSel.mask;   // native RAM keeps what it had
        }
}

// setup(), after enableBus() put the real map in (mmuStateSaved keeps it),
// before core 1 starts: the flood's pages and blocks, the stub, and core
// 1's first map the transparent one.
static void sledPrepare() {
    static const uint8_t stub[] = {
        0x78,               // 0600 SEI
        0xd8,               // 0601 CLD
        0xa9, 0x00,         // 0602 LDA #0
        0x8d, 0x0e, 0xd4,   // 0604 STA NMIEN: no more VBIs or DLIs
        0xa9, kSledLandedMark, // 0607 LDA #$A5
        0x8d, 0xff, 0x06,   // 0609 STA $06FF: in the stub
        0xad, 0xfe, 0x06,   // 060C LDA $06FE
        0xf0, 0xfb,         // 060F BEQ $060C: until main says go
        0xa9, 0xff,         // 0611 LDA #$FF
        0x8d, 0x01, 0xd3,   // 0613 STA PORTB: the OS ROM in, the main bank
        0x6c, 0xfc, 0xff,   // 0616 JMP ($FFFC): the reset handler
    };
    memset(sledPage, kSledFlood, sizeof(sledPage));
    memset(sledZeroPage, kSledFlood, sizeof(sledZeroPage));
    memcpy(atariRam + kSledStub, stub, sizeof(stub));
    atariRam[kSledGo] = 0;
    atariRam[kSledLanded] = 0;
    sledLow = (L1Block *)heap_caps_malloc(sizeof(L1Block), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    sledAll = (L1Block *)heap_caps_malloc(sizeof(L1Block), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(sledLow && sledAll);
    sledFill(sledLow, true);
    sledFill(sledAll, false);
    mmuState.safeCopy(mmuStateDisabled);   // core 1 starts transparent
    LOG("SLED_TEST: core 1 starts on the transparent map; the $4C flood ready, the stub at $%04x\n", kSledStub);
}

#ifdef CSIM
extern "C" uint16_t CPU_regPC;          // atari800's 6502, for the timeout's report
extern "C" uint8_t CPU_regP, CPU_regS;
extern "C" uint8_t MEMORY_mem[];        // atari800's memory (the screen at the flood)
extern "C" void CPU_CSIM_TraceDumpN(const char *why, unsigned int max);   // atari800's instruction trace
#endif
// Spin for at most `ticks` (CCOUNT) while `wait()`; the write ring is
// drained (no core0Loop() meanwhile: nothing in it to act on), the read
// ring is wait()'s to read. Returns the ms spun.
template <typename Wait>
static uint32_t sledSpin(uint32_t ticks, Wait wait) {
    const uint32_t t0 = XTHAL_GET_CCOUNT();
    while (wait() && XTHAL_GET_CCOUNT() - t0 < ticks)
        bmonTail = bmonHead;
    return (XTHAL_GET_CCOUNT() - t0) / 240000;   // ms
}

// The loop at $4C4C in the read ring: kSledLoopReads reads in a row, all
// of $4C4C-$4C4E (three per JMP), from the reads since the last look (the
// newest part of a lapped ring).
static constexpr int kSledLoopReads = 24;
static int sledLoopRun = 0;
static unsigned int sledRdHead0, sledWrHead0;   // the ring heads as the flood went live
static bool sledLoopSeen() {
    const unsigned int head = bmonRdHead;
    if (head - bmonRdTail > bmonRdArraySz) bmonRdTail = head - bmonRdArraySz;
    for (; bmonRdTail != head; bmonRdTail++) {
        const uint16_t addr = r0ToAddr(bmonRdArray[bmonRdTail & bmonRdArraySzMask].r0 >> bmonR0Shift);
        sledLoopRun = (addr - kSledLoop) < 3u ? sledLoopRun + 1 : 0;
        if (sledLoopRun >= kSledLoopReads) return true;
    }
    return false;
}

// The newest bus cycles core 1 logged (addresses only: the rings hold no
// data), copied first, then printed: what the 6502 was doing at a catch or
// a timeout.
static uint16_t sledR0Addr(uint32_t r0) {
    return r0ToAddr(r0 >> bmonR0Shift);
}
static void sledRingDump(const char *why) {
    constexpr int kReads = 16, kWrites = 8;
    uint16_t rd[kReads], wr[kWrites];
    const unsigned int rh = bmonRdHead, wh = bmonHead;
    for (int i = 0; i < kReads; i++) rd[i] = sledR0Addr(bmonRdArray[(rh - kReads + i) & bmonRdArraySzMask].r0);
    for (int i = 0; i < kWrites; i++) wr[i] = sledR0Addr(bmonArray[(wh - kWrites + i) & bmonArraySzMask].r0);
    char line[160];
    int n = 0;
    for (int i = 0; i < kReads; i++) n += snprintf(line + n, sizeof(line) - n, " %04x", rd[i]);
    LOG("SLED_TEST: %s: last reads%s\n", why, line);
    n = 0;
    for (int i = 0; i < kWrites; i++) n += snprintf(line + n, sizeof(line) - n, " %04x", wr[i]);
    LOG("SLED_TEST: %s: last writes%s (%u reads, %u writes logged since the flood)\n", why, line,
        rh - sledRdHead0, wh - sledWrHead0);
}

// threadFunc(), the bus hot: memory and mmuState only.
static void sledLanding() {
    sledSpin(240u * 1000 * 5, [] { return true; });   // 5 ms of warm-up on the transparent map
    bmonRdTail = bmonRdHead;
    sledLoopRun = 0;
#ifdef CSIM
    LOG("SLED_TEST: the flood goes live; the 6502 at PC $%04x, P $%02x, S $%02x\n", CPU_regPC, CPU_regP, CPU_regS);
    CPU_CSIM_TraceDumpN("SLED_TEST: the flood goes live", 24);
    {   // the screen as atari800 has it (native RAM: main can't read it)
        const uint16_t sav = MEMORY_mem[0x58] | (MEMORY_mem[0x59] << 8);
        for (int row = 0; row < 24; row++) {
            char line[41];
            bool any = false;
            for (int i = 0; i < 40; i++) {
                const uint8_t c = MEMORY_mem[(uint16_t)(sav + row * 40 + i)] & 0x7f;
                line[i] = c < 64 ? c + 32 : c < 96 ? c - 64 : c;
                if (line[i] < 32 || line[i] > 126) line[i] = '.';
                any |= c != 0;
            }
            line[40] = 0;
            if (any) LOG("SLED_TEST: screen %2d |%s|\n", row, line);
        }
    }
#endif
    sledRdHead0 = bmonRdHead;
    sledWrHead0 = bmonHead;
    for (auto &b : mmuState.extBanks) b = sledAll;     // the flood live, a pointer at a time
    mmuState.l1Blocks[0] = sledLow;
    // halt_ released by core 1 every cycle from here, as core0Loop()'s
    // start has it (no startup halt to release: setup() leaves the pin
    // undriven; a halting write's own bit still asserts it).
    mmuState.pinReleaseMask |= bus.halt_.mask;
    // 3 s of the bus clock running (an Atari still off, its power just
    // switched on, isn't counted: the bmon heads move only with phi2), 30 s
    // at most.
    uint32_t looping;
    {
        const uint32_t t0 = XTHAL_GET_CCOUNT();
        uint32_t window = t0, clocked = 0;
        unsigned int heads = bmonHead + bmonRdHead;
        while (!sledLoopSeen() && clocked < 3000 && XTHAL_GET_CCOUNT() - t0 < 240u * 1000 * 30000) {
            bmonTail = bmonHead;
            if (XTHAL_GET_CCOUNT() - window >= 240000) {   // a 1 ms window
                window += 240000;
                const unsigned int h = bmonHead + bmonRdHead;
                if (h != heads) clocked++;
                heads = h;
            }
        }
        looping = (XTHAL_GET_CCOUNT() - t0) / 240000;
    }
    const bool caught = sledLoopRun >= kSledLoopReads;
    sledRingDump(caught ? "caught" : "not caught");
    // Memory core 1 and the 6502 change under us: volatile, or the compiler
    // reads the stub's mark once and never again (atariRam isn't volatile:
    // RAM_VOLATILE, mmu.h), and may move the patch's stores.
    volatile uint8_t *const ram = atariRam;
    volatile uint8_t *const flood = sledPage;
    uint32_t stubbed = 0;
    bool inStub = false;
    if (caught) {
        // JMP $4C4C -> $4C00 (the flood: back to $4C4C) -> $0600, a byte at a time.
        flood[(kSledLoop + 1) & pageOffsetMask] = kSledStub & 0xff;
        flood[(kSledLoop + 2) & pageOffsetMask] = kSledStub >> 8;
        stubbed = sledSpin(240u * 1000 * 100, [ram] { return ram[kSledLanded] != kSledLandedMark; });
        inStub = ram[kSledLanded] == kSledLandedMark;
        if (!inStub) sledRingDump("not in the stub");
    }
    ram[0x033d] = 0;   // PUPBT1: no warm start
    ram[0x0244] = 1;   // COLDST: a cold start
    enableBus();            // the real map back (the stub's page 6 is in it too)
    if (irqJammed) irqUnjam(true);
    busyWait6502Ticks(10);
    ram[kSledGo] = 1;
    LOG("SLED_TEST: %s after %u ms, %s after %u ms more; the real map back, the stub let go\n",
        caught ? "looping at $4C4C" : "NOT CAUGHT (timed out)", (unsigned)looping,
        inStub ? "in the stub" : "NOT IN THE STUB", (unsigned)stubbed);
#ifdef CSIM
    if (!inStub) LOG("SLED_TEST: the 6502 at PC $%04x, P $%02x, S $%02x\n", CPU_regPC, CPU_regP, CPU_regS);
#endif
}
#endif

IRAM_ATTR void resume6502() { 
    bmonTail = bmonHead;    
    // Release halt_ for two read cycles, not a blind 15 ticks: a halting
    // write inside the old window lost its halt (the resume-halt race).
    // Core 1 applies pinReleaseMask at each cycle's P1; the read ring's
    // head, noted after setting the bit, passes 2 once a read cycle that
    // loaded the bit has finished (the first may have loaded it before).
    // The bit is gone again ~2 cycles after the release; the 6502's next
    // halting write (STA abs) is at least 4 cycles after its halted read.
    // Core 1 is untouched: tried and dropped (2026-11-19), core 1 clearing
    // the bit in its read tail broke .1's boot (an RDY edge mid-cycle, a
    // store loaded by the next P1) and the bench's FAKE_SIO bank test.
    // Bounded (20 us), as the blind wait was.
    mmuState.pinReleaseMask |= bus.halt_.mask;
    const unsigned int mark = bmonRdHead;
    const uint32_t t0 = XTHAL_GET_CCOUNT();
    while (bmonRdHead - mark < 2U && XTHAL_GET_CCOUNT() - t0 < 240U * 20U) {}
    mmuState.pinReleaseMask &= haltMaskNOT;
    if (bmonRdHead - mark < 2U)
        resumeTimeouts++;
}

static DRAM_ATTR uint8_t savedCartBank = 0;
static constexpr DRAM_ATTR int page01 = pageNr(0x100);
static constexpr DRAM_ATTR int l1Block01 = page2L1Block(page01);

IRAM_ATTR void enableBus() {
    //busWriteDisable = 0;
    //pinEnableMask = _0xffffffff;
    busyWait6502Ticks(20);
    cartBankLatch = savedCartBank;
    mmuState.safeCopy(mmuStateSaved); 
    busyWait6502Ticks(10);
//#define STACKPROG_PLUS_HALT
#ifdef STACKPROG_PLUS_HALT 
    for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
        l1Blocks[l1Block01].ctrl[(page01 & pageInL1BlockMask) + PAGESEL_RD + vid] &= bus.halt_.maskInverse;
    }
    busyWait6502Ticks(10);
    mmuState.pinReleaseMask |= bus.halt_.mask;
    busyWait6502Ticks(10);
    mmuState.pinReleaseMask &= haltMaskNOT;
    //busyWait6502Ticks(1000);
#endif
}


// disableBus -// replace all l1Blocks[] pointers with pointers to dummy blocks
// bus core1.cpp loop is still running and will continue to update l1Blocks[] pointers
// and may even mistakenly interpret mis-timed bus data as IO to cartctl region and
// update basicEnL1BlockMux[] from cartBanks[].   Thus everything
// l1Blocks[], basicEnL1BlockMux[], osEnL1BlockMux[], and cartBanks[] all need to be
// replaced with dummy blocks.

IRAM_ATTR void disableBus() {
    mmuStateSaved.safeCopy(mmuState);
#ifdef STACKPROG_PLUS_HALT 
    assert(config.haltAvailable == true);
    //mmuState.pinReleaseMask &= bus.halt_.maskInverse;
    for(int vid : PAGESEL_EXTRA_VARIATIONS) {  
        l1Blocks[l1Block01].ctrl[(page01 & pageInL1BlockMask) + PAGESEL_RD + vid] |= bus.halt_.mask;
    }
    // TODO - if this works, just initialize and keep dummyL1Block.ctrl intialized this way, maybe rename it to
    // disabledBank or something 
    static DRAM_ATTR int firstrun = 1;
    if (config.haltAvailable && firstrun) { 
        firstrun = 0;
        for(auto &c : dummyL1Block.ctrl) c = bus.halt_.mask;
    }
    //busyWait6502Ticks(120);  // why does this cause immediate 13.8sec stackprog timeouts?
#endif    
    mmuState.safeCopy(mmuStateDisabled);
    savedCartBank = cartBankLatch;
    //busWriteDisable = 1;
    //pinEnableMask = bus.halt_.mask;
    //busyWait6502Ticks(20);
}

DRAM_ATTR static const int psram_sz =  32 * 1024;
DRAM_ATTR uint32_t *psram;
DRAM_ATTR uint32_t *psram_end;

DRAM_ATTR static const int testFreq = fakeClockHz;   // fakeBus.h
// SysConfig::save() (sysConfig.h): here, not in sysConfig.cpp, which the
// Sidecar app builds too and which has no Sio.
void SysConfig::save() {
    int fd = ::open(sio.resolve("/config.txt").c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return;
    if (::write(fd, cartImage.c_str(), cartImage.length()) < 0) {}   // best effort
    ::close(fd);
}

DRAM_ATTR static const int lateThresholdTicks = 180 * 2 * 1000000 / testFreq;
static const DRAM_ATTR uint32_t halfCycleTicks = 240 * 1000000 / testFreq / 2;

//  socat TCP-LISTEN:9999 - > file.bin
bool sendPsramTcp(const char *buf, int len, bool resetWdt = false) { 
#if 0
    //char *host = "10.250.250.240";
    char *host = "192.168.68.131";
    //wifiConnect();
    wifiDisconnect();
    wifiConnect();
    WiFiClient wc;
    static const int txSize = 1024;
   
    int r = wc.connect(host, 9999);
    LOG("connect() returned %d\n", r);
    uint32_t startMs = millis();
    int count;
    int sent = 0;
    while(sent < len) { 
        if (!wc.connected()) { 
            LOG("lost connection");
            return false;
        }
        int pktLen = min(txSize, len - sent);
        r = wc.write((uint8_t *)(buf + sent), pktLen);
        if (r != pktLen) {
            LOG("write %d returned %d\n", count, r);
            return false;
        }
        sent += r;
        if (count++ % 100 == 0) { 
            LOG("."); 
            fflush(stdout);
        }
        if (resetWdt) wdtReset();
        yield();
    }
    LOG("\nDone %.3f mB/sec\n", psram_sz * sizeof(psram[0]) / 1024.0 / 1024.0 / (millis() - startMs) * 1000.0);
    fflush(stdout);
#endif
    return true;
}


DRAM_ATTR int ramReads = 0, ramWrites = 0;

struct DRAM_ATTR { 
    char buf[64]; // must be power of 2
    int head = 0, tail = 0;
    inline IRAM_ATTR bool available() { return head != tail; }
    inline IRAM_ATTR uint8_t getKey() { 
        if (head == tail) return 0;
        uint8_t c = buf[tail];
        tail = (tail + 1) & (sizeof(buf) - 1);
        if (c == 255) c = 0;
        if (c == '\n') c = '\233';
        return c;
    }
    inline IRAM_ATTR void putKey(char c) { 
            buf[head] = c;
            head = (head + 1) & (sizeof(buf) - 1); 
    }
    inline IRAM_ATTR void putKeys(const char *p) {  
        while(*p != 0) { 
            buf[head] = *p++;
            head = (head + 1) & (sizeof(buf) - 1); 
        }
    }
    inline IRAM_ATTR void putKeys(const char *p, int len) { 
        for(int n = 0; n < len; n++) {
            buf[head] = p[n];
            head = (head + 1) & (sizeof(buf) - 1); 
        }
    }
} simulatedKeyInput;

void putKeys(const char *s, int len) { 
    simulatedKeyInput.putKeys(s, len);
}
void putKey(char c) { 
    simulatedKeyInput.putKey(c);
}
DRAM_ATTR int lastScreenShot;
DRAM_ATTR int secondsWithoutWD = 0, lastIoSec = 0;
DRAM_ATTR StructLogs *structLogs = NULL;

// Turn the struct logs on; until then structLogs is null and every user
// skips them. Internal RAM, never PSRAM: core 0 adds to these rings with
// core 1 hot, and a PSRAM access goes through the cache and MSPI bus that
// core 1 shares. Plain new would land in PSRAM
// (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is 0, so every malloc prefers PSRAM).
void structLogsInit()
{
    if (structLogs) {
        return;
    }
    if (void *mem = heap_caps_malloc(sizeof(StructLogs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
        structLogs = new (mem) StructLogs();
#ifndef CSIM
        assert(esp_ptr_internal(structLogs));
#endif
    } else {
        LOG("structLogs: no %u bytes of internal RAM, struct logs off\n",
               (unsigned)sizeof(StructLogs));
    }
}

// The Atari half of this boot's configuration (sysConfig.h): a -DBOOT_CONFIG
// profile compiled in wins (config.load() has it, both halves); else the
// Sidecar's choice (SidecarObject::Config: it owns the configuration files),
// else, with no Sidecar answering, main's own files (/spiffs/config, as a
// standalone build reads them), else SysConfig::kFallback (load()'s).
// What chooseBootConfig() chose, for its log line and again beside BOOT
// COMPLETE (a USB console's first lines are lost before a host reads them).
static std::string bootConfigSummary;

static void chooseBootConfig()
{
    if (!config.bootConfig.empty()) {
        bootConfigSummary = config.bootConfig + " (built in: -DBOOT_CONFIG)";
        LOG("boot config: %s\n", bootConfigSummary.c_str());
        return;
    }
#if A8BB_SIDECAR
    std::string text;
    switch (sidecarHost.fetchConfig(text)) {
    case SidecarHost::ConfigFetch::Got: {
        AtariConfig chosen;
        std::vector<AtariConfig::Error> errors;
        chosen.fromText(text.c_str(), &errors);
        for (const auto &e : errors) LOG("boot config: line %d: %s\n", e.line, e.msg.c_str());
        static_cast<AtariConfig &>(config) = chosen;
        const size_t nl = text.find('\n');
        static constexpr const char kHeader[] = "# boot config: ";   // bootConfigChoose()'s first line
        const size_t skip = text.rfind(kHeader, 0) == 0 ? sizeof(kHeader) - 1 : 0;
        bootConfigSummary = text.substr(skip, nl - skip) + ", from the Sidecar";
        LOG("boot config: %s\n", bootConfigSummary.c_str());
        return;
    }
    case SidecarHost::ConfigFetch::None:
        bootConfigSummary = std::string(SysConfig::kFallback) + " (the Sidecar has none chosen)";
        LOG("boot config: %s\n", bootConfigSummary.c_str());
        return;
    case SidecarHost::ConfigFetch::NoSidecar:
        break;   // main's own files
    }
#endif
    AtariConfig chosen;
    BootConfigChoice choice;
    const bool ok = bootConfigFiles::choose("/spiffs/config", chosen, choice);
    for (const auto &e : choice.errors) LOG("boot config: line %d: %s\n", e.line, e.msg.c_str());
    if (!ok) {
        bootConfigSummary = std::string(SysConfig::kFallback) + " (" + choice.how + ", /spiffs/config)";
        LOG("boot config: %s\n", bootConfigSummary.c_str());
        return;
    }
    static_cast<AtariConfig &>(config) = chosen;
    bootConfigSummary = choice.name + " (" + choice.how + ", /spiffs/config)";
    LOG("boot config: %s\n", bootConfigSummary.c_str());
}

void setup();

static uint8_t *setupAtariRam = nullptr;
static size_t setupAtariRamSize = 0;
static bool setupComplete = false;
const static DRAM_ATTR uint32_t bmonTimeout = 240 * 1000 * 10;


#include "lwip/sys.h"

DRAM_ATTR int bmonCaptureDepth = 0;
const static DRAM_ATTR int prerollBufferSize = 32; // must be power of 2
DRAM_ATTR uint32_t prerollBuffer[prerollBufferSize]; 
DRAM_ATTR uint32_t prerollIndex = 0;
DRAM_ATTR uint32_t *psramPtr;

bool IRAM_ATTR bmonExclude(uint32_t bmon) { 
    for(int i = 0; i < sizeof(bmonExcludes)/sizeof(bmonExcludes[0]); i++) { 
        if ((bmon & bmonExcludes[i].mask) == bmonExcludes[i].value) 
            return true;
    }
    return false;
}

void IRAM_ATTR bmonAddToPsram(uint32_t bmon) { 
    *psramPtr = bmon;
    psramPtr++;
    if (psramPtr == psram_end) 
        psramPtr = psram;
}

void IRAM_ATTR bmonLog(uint32_t bmon) {
    const static DRAM_ATTR uint32_t bmonMask = 0x2fffffff;
#ifdef HAVE_REFRESH
    if ((bmon & (bus.refresh_.mask << bmonR0Shift)) == 0)
        return;
#endif

    if (bmonCaptureDepth > 0) {
        if (!bmonExclude(bmon)) {
            bmonCaptureDepth--;
            bmonAddToPsram(bmon & bmonMask);
            return;
        }
    } else { 
        for(int i = 0; i < sizeof(bmonTriggers)/sizeof(bmonTriggers[0]); i++) { 
            BmonTrigger &t = bmonTriggers[i];
            // for(auto &t : bmonTriggers) {
            if (t.count > 0 && t.depth > 0 && (bmon & t.mask) == t.value) {
                if (t.skip > 0) { 
                    t.skip--;
                } else {
                    bmonCaptureDepth = t.depth - 1;
                    t.count--;
                    for(int i = min(prerollBufferSize, t.preroll); i > 0; i--) { 
                        // Compute backIdx as prerollIndex - i;
                        int backIdx = (prerollIndex + (prerollBufferSize - i)) & (prerollBufferSize - 1);
                        if (bmonExclude(prerollBuffer[backIdx]))
                            continue;
                        bmonAddToPsram(bmon);
                    }
                    bmon = (bmon & bmonMask) | (0x80000000 | t.mark);
                    t.mark = 0; 
                    bmonAddToPsram(bmon);
                    bmonAddToPsram(XTHAL_GET_CCOUNT());
                    return;
                }
            }
        }
    }
    prerollBuffer[prerollIndex] = bmon & bmonMask;
    prerollIndex = (prerollIndex + 1) & (prerollBufferSize - 1); 
}

void IRAM_ATTR core0LowPriorityTasks(); 
DRAM_ATTR int consecutiveBusIdle = 0;
DRAM_ATTR volatile int sysMonitorTime = 10;

const static DRAM_ATTR uint32_t bmonMask = 0x2fffffff;


// Toy implementation of the RTime8 real time clock.  TODO: set hardcoded time/date values
// based on NTP server data or something.  
static DRAM_ATTR struct RTime8Clock { 
    uint8_t cmd = 0;
    int nibble = 0;
    uint8_t response[16][4] = {
        {0, 1, 2, 0},
        {0, 3, 4, 0},
        {0, 1, 2, 0},
        {0, 2, 5, 0},
        {0, 0, 6, 0},
        {0, 7, 2, 0},
        {0, 0, 2, 0},
    };
    __attribute__((always_inline)) void read(uint8_t *rdReg) {
        if (nibble == 1) nibble = 2;
        else if (nibble == 2) nibble = 0;
        *rdReg = response[cmd][nibble];
    }
    __attribute__((always_inline)) void write(uint8_t *rdReg, uint8_t *wrReg) {
        if (nibble == 0) {
            cmd = *wrReg & 0xf;
            nibble = 1;
            *rdReg = response[cmd][nibble];
        } else { 
            //printf("wrote %d -> cmd %d nibble %d\n", d000Write[_0x5b9] & 0xf, rtc.cmd, rtc.nibble);
            response[cmd][nibble] = *wrReg & 0xf;
            if (nibble == 1) nibble = 2;
            else if (nibble == 2) nibble = 0;
        }
    }
} rtime8;

// One bmon entry. This and core0_utilityTasks() must never fall behind the
// ring except inside handlePbiRequestHot(): see the rule at bmonArray (bmon.h).
void CORE0_HOTSPOTS_ATTR core0_onBmonChange() { 
    unsigned int bHead = bmonHead;
    if (bHead == bmonTail) {
#ifdef FAKE_SIO
        bmonFakeWrBacklog = false;   // FAKE_SIO's step's backlog drained
#endif
        return;
    }
    const unsigned int depth = bHead - bmonTail;
    const uint32_t now = CSIM_PEEK_CCOUNT();
#ifdef FAKE_SIO
    if (bmonFakeWrBacklog) {   // the test harness's own time: counted apart
        if (depth > bmonFakeWrHwm) bmonFakeWrHwm = depth;
        if (depth > bmonArraySz) bmonFakeLaps++;
    } else
#endif
    {
        ++bmonWrDepths[bmonDepthBucket(depth)];
        if (depth > bmonWrHwm) {   // before the clamp
            bmonWrHwm = depth;
            bmonWrMaxTag = BmonMaxTag{now - bmonWrLastDrain, core0PhaseAddr, core0Phase};
        }
        if (depth > bmonArraySz) bmonLaps++;
    }
    bmonWrLastDrain = now;
    if (depth > bmonArraySz) {
        // Lapped (writes lost): go on from the newest entries, short of the
        // one core 1 may be writing.
        bmonTail = bHead - (bmonArraySz - 16);
    }

    PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];

    //bmonMax = max((bHead - bmonTail) & bmonArraySzMask, bmonMax);
    uint32_t bmon = bmonArray[bmonTail & bmonArraySzMask].r0 & bmonMask;
    // Consume the entry now, not at the end: handlers below that discard the
    // backlog (bmonTail = bmonHead, here or in resume6502()) must have the
    // last word. A closing bmonTail++ after them left the tail one past the
    // head, and core 0 then replayed the whole stale ring (~511 old entries,
    // racing core 1's overwrites), re-running mmuOnChange() on stale writes.
    bmonTail++;

    uint32_t r0 = bmon >> bmonR0Shift;

    uint16_t addr = r0ToAddr(r0);
    if ((r0 & bus.rw.mask) == 0) {   // always: the write ring (reads: core0_onBmonReads())
        CORE0_PHASE(PH_WRITE, addr);
#ifdef HAVE_REFRESH
        if ((r0 & bus.refresh_.mask) == 0)   // never a refresh cycle (writesRefreshLow)
            writesRefreshLow++;
#endif
        uint32_t lastWrite = addr;
        if ((lastWrite & _0xff00) == _0xd500 && atariCart.accessD500(lastWrite)) {
        } else if (lastWrite == _0xd830 && pbiRequest[0].req != 0) {
            handlePbiRequestHot(&pbiRequest[0]);
            bmonTail = bmonHead;
        } else if (lastWrite == _0xd840 && pbiRequest[1].req != 0) {
            handlePbiRequestHot(&pbiRequest[1]);
            bmonTail = bmonHead;
        }
#define RTIME8_CLOCK
#ifdef RTIME8_CLOCK
        else if(lastWrite == _0xd5b9) {
            rtime8.write(&d000Read[_0x5b9], &d000Write[_0x5b9]);
        }
#endif
        // The halting pages ($D3xx, $D1FF's): act on the write, then resume
        // the 6502. Tried (2026-11-19) and not kept: first waiting for the
        // halted 6502's read (the read ring's head passing a mark taken at
        // the write), so the remap would see an RMW's second write too; it
        // failed the bench's FAKE_SIO bank test (canary errors, even with
        // only the wait), unexplained; the bench's fake 6502 never halts,
        // so an artifact is suspected but not proven.
        if (pageNr(lastWrite) == pageNr_d301 || pageNr(lastWrite) == pageNr_d1ff) {
            if (lastWrite == _0xd301) {
                mmuState.l1Blocks[l1Block40] = mmuState.extBanks[d000Write[_0x301]];
                mmuOnChange();
            } else if (lastWrite == _0xd1ff) {
                mmuOnChange();
            }
            bmonTail = bmonHead;
            // Don't know why it hangs without this wait (it was
            // bmonWaitCycles(2), 2 bus cycles counted in bmon). Still very
            // much the case on both 130XE and 600XL.
            //busyWait6502Ticks(2);
            if (config.haltAvailable) {
                haltCount++;
                resume6502();
            }
        }
        // repeatedBrokenRead detection is disabled below.
    }
#ifdef HAVE_REFRESH
    else if ((r0 & bus.refresh_.mask) != 0) {
#else
    else { 
#endif  // HAVE_REFRESH
     
        // (Reads are in their own ring now: R-Time 8's are handled in
        // core0_onBmonReads().)
#if 0  // repeatedBrokenRead seems to get triggered on the 600XL when the 6502 is halted during stackprog
        uint16_t lastRead = addr;
        #ifdef FAKE_CLOCK
        if (addr == 0x180) break;
        #endif
        DRAM_ATTR static uint16_t lastLastRead = 0;
        if (lastRead == lastLastRead) { 
            repeatedBrokenRead++;
            if (repeatedBrokenRead > 300 && elapsedSec > 20) {
                core0_exit(1, "-4 6502 repeat reads %04x", lastRead);
                break;
            }
        } else { 
            repeatedBrokenRead = 0;
        }
        lastLastRead = lastRead;
        //if ((lastRead & _0xff00) == 0xd500 && atariCart.accessD500(lastRead)) 
        //    onMmuChange();
        //if (bankNr(lastWrite) == pageNr_d500)) resume6502(); 
        //if (lastRead == 0xFFFA) lastVblankTsc = XTHAL_GET_CCOUNT();
#endif
    }    
    //PROFILE_BMON((bHead - bmonTail) & bmonArraySzMask);

#if 0  // this should be do-nothing code, why does it destroy core0 loop timing after
// heavy interrupts
    if (bmonCaptureDepth > 0) {
        bool skip = false;
        for(int i = 0; i < sizeof(bmonExcludes)/sizeof(bmonExcludes[0]); i++) { 
            if ((bmon & bmonExcludes[i].mask) == bmonExcludes[i].value) {
                skip = true;
                break;
            }
        }
        if (skip) 
            continue;
        bmonCaptureDepth--;
        *psramPtr = bmon;
        psramPtr++;
        if (psramPtr == psram_end) 
            psramPtr = psram; 
    } else { 
        for(int i = 0; i < sizeof(bmonTriggers)/sizeof(bmonTriggers[0]); i++) { 
//                for(auto &t : bmonTriggers) {
            BmonTrigger &t = bmonTriggers[i];
            if (t.count > 0 && t.depth > 0 && (bmon & t.mask) == t.value) {
                if (t.skip > 0) { 
                    t.skip--;
                } else {
                    bmonCaptureDepth = t.depth - 1;
                    t.count--;
                    for(int i = min(prerollBufferSize, t.preroll); i > 0; i--) { 
                        // Compute backIdx as prerollIndex - i;
                        int backIdx = (prerollIndex + (prerollBufferSize - i)) & (prerollBufferSize - 1);
                        bool skip = false;
                        for(int i = 0; i < sizeof(bmonExcludes)/sizeof(bmonExcludes[0]); i++) { 
                            if ((prerollBuffer[backIdx] & bmonExcludes[i].mask) == bmonExcludes[i].value) {
                                skip = true;
                                break;
                            }
                        }
                        if (skip) 
                            continue;
                        *psramPtr = prerollBuffer[backIdx];
                        psramPtr++;
                        if (psramPtr == psram_end) 
                            psramPtr = psram; 
                    }

                    bmon |= (0x80000000 | t.mark | busEnabledMark);
                    t.mark = 0; 
                    *psramPtr = bmon;
                    psramPtr++;
                    if (psramPtr == psram_end) 
                        psramPtr = psram;
                    *psramPtr = XTHAL_GET_CCOUNT();
                    psramPtr++;
                    if (psramPtr == psram_end) 
                        psramPtr = psram;
                    break;
                }
            }
        }
        if (bmonCaptureDepth > 0)
            continue;
    }
    #ifdef BMON_PREROLL
    prerollBuffer[prerollIndex] = bmon;
    prerollIndex = (prerollIndex + 1) & (prerollBufferSize - 1); 
    #endif
#endif // #if 0
    if (bmonHead == bmonTail) CSIM_CORE0_YIELD();
}

// The read ring (bmon.h), best effort: called after each write (the loop
// alternates), bmonRdBatch entries at a time, so a write waits behind at
// most that many reads. Only R-Time 8's $D5B9 reads matter (each advances
// its nibble sequence).
void CORE0_HOTSPOTS_ATTR core0_onBmonReads() {
    const unsigned int rHead = bmonRdHead;
    const unsigned int depth = rHead - bmonRdTail;
#ifdef FAKE_SIO
    if (depth == 0)
        bmonFakeRdBacklog = false;
    if (bmonFakeRdBacklog) {
        if (depth > bmonFakeRdHwm) bmonFakeRdHwm = depth;
        if (depth > bmonRdArraySz) bmonFakeRdLaps++;
    } else
#endif
    {
        const uint32_t now = CSIM_PEEK_CCOUNT();
        if (depth > 0) ++bmonRdDepths[bmonDepthBucket(depth)];
        if (depth > bmonRdHwm) {   // before the clamp
            bmonRdHwm = depth;
            bmonRdMaxTag = BmonMaxTag{now - bmonRdLastDrain, core0PhaseAddr, core0Phase};
        }
        if (depth > bmonRdArraySz) bmonRdLaps++;   // reads lost: fine (see bmon.h)
        bmonRdLastDrain = now;
    }
    // No CORE0_PHASE() here: a read drain is short (bmonRdBatch reads), and
    // with the rings alternating it runs between a write's handling and
    // the next write drain, so marking it would hide that write (a $D1FF
    // write's ~10 us showed as "reads").
    if (depth > bmonRdArraySz)
        bmonRdTail = rHead - (bmonRdArraySz - 16);
    // Not unrolled: IRAM is tight (GCC's unrolled copy was ~1 KB, and
    // pushed .data, core 0's interrupt stack with it, into core 1's hot
    // data bank: verify_core1_hot_data).
#pragma GCC unroll 1
    for (int n = 0; n < bmonRdBatch && bmonRdTail != rHead; ++n) {
        const uint32_t r0 = (bmonRdArray[bmonRdTail & bmonRdArraySzMask].r0 & bmonMask) >> bmonR0Shift;
        bmonRdTail++;   // (refresh cycles aren't in the ring: core 1 skips them)
#ifdef RTIME8_CLOCK
        if (r0ToAddr(r0) == _0xd5b9) {
            rtime8.read(&d000Read[_0x5b9]);
        }
#endif
    }
}
// Core 0's housekeeping between bus monitor passes: simulated key input, PBI
// interrupts, the once-a-second checks (elapsedSec, the watchdog, exit
// conditions). core0Loop() calls it once per bmonTimeout pass (about every
// 50 ms) on hardware, and csim_board_tick() calls it at the same interval.
// Always inlined, so core0Loop() keeps the code it had when this was
// written out in its loop. An exit condition calls core0_exit() (exitReason
// and exitFlag) and returns true at once, so the first one to fire names
// the exit and core0Loop() breaks out of its loop, as the inline code did.
// csim, which keeps calling this, ends the run on exitFlag.
// PBI requests core0_utilityTasks()'s poll handled because bmon never
// dispatched them (reported at the end of the run).
DRAM_ATTR int pbiPollCatches = 0;
#ifndef CSIM
// Core 1's watchdog: core 0 checks once a second that core 1 still serves
// the bus (the bmon heads move: core 1 pushes every bus cycle but refresh
// into one ring or the other, the 6502 halted or not). Quiet for
// kCore1QuietSec with phi2 running, core 1 is dead: core 0 ends the run
// with a failure exit (core0_exit(), "-10 CORE1_FAULT ..." with the fault,
// or "-10 CORE1_HANG ..." with core 1's PC from the assist-debug unit), so
// the exit report, DONE, then EXIT_COREDUMP's panic: a core dump, the
// reboot and the Sidecar's fetch. A core 1 fault used to leave main hung:
// core 1 runs on the ROM's vectors, whose fatal handler prints on the ROM
// UART and spins (ets_fatal_exception_handler), so no reboot, no boot
// report and no core. core1ExceptionHandler() (below) records it instead.
// Quiet with phi2 stopped is an Atari powered off: logged, not a failure.
// A long PBI request can't fake it: the once-a-second block fires once on
// its return, not once per second missed.
struct Core1Fault {
    uint32_t valid;   // set last, by core1ExceptionHandler()
    uint32_t pc, exccause, excvaddr;
};
DRAM_ATTR volatile Core1Fault core1Fault = {};
static constexpr int kCore1QuietSec = 3;
DRAM_ATTR static unsigned core1WatchHeads = 0;
DRAM_ATTR static int core1QuietSec = 0;
DRAM_ATTR static bool core1ClockStoppedLogged = false;
static void core1Stalled();   // below (flash: core 1 isn't on the bus)
#endif
#if (defined(TEST_CORE1_FAULT) || defined(TEST_CORE1_HANG)) && !defined(CSIM)
DRAM_ATTR bool testCore1Armed = false;   // setup(): this boot didn't follow a panic
#endif
#if defined(TEST_PANIC) && TEST_PANIC > 0 && !defined(CSIM)
DRAM_ATTR bool testPanicArmed = false;   // setup(): this boot didn't follow a panic
// The wild pointer, through a volatile so the compiler can't see it's bad.
DRAM_ATTR volatile uintptr_t testPanicAddr = 4;
#endif


#if A8BB_SIDECAR && !defined(CSIM)
// Diagnostic mode: for the Sidecar's work that reads or writes main's flash
// (fetching a core dump, a firmware update). Entered only at boot, from
// setup(), when the Sidecar's reply to the boot report (BootStatus) asks for
// it: before core 1 starts or the bus is ever attached, so a running Atari
// is simply never served (it crashes, which is accepted: either main just
// crashed, which is why there's a core dump, or the Atari isn't powered on
// yet). The Sidecar can't put a running main in diagnostic mode; to force
// it, it resets main and answers the next boot report. From here on core 0
// only serves the link, the core dump (Core) included. Only a reset leaves
// it. The hot bmon loop never polls the link at all.
// Planned hardware: main will also hold the Atari's reset line low here, so
// an Atari left running unserved can't hurt itself. No reset line on the
// current boards yet.
// The flags (SidecarDiagnosticFlag):
//   Panic: abort(), a deliberate crash to test core dumps (the panic
//     handler writes one and reboots).
//   FetchFirmware: pull the Sidecar's Firmware object into the next app
//     slot and restart into it (SidecarHost::installFirmware()).
[[noreturn]] static void sidecarDiagnosticMode() {
    const uint8_t flags = sidecarHost.diagnosticFlags();
    LOG("Diagnostic mode (Sidecar, flags %02x): bus never attached; serving the link until reset\n",
           flags);
    fflush(stdout);
    if (flags & static_cast<uint8_t>(SidecarDiagnosticFlag::Panic)) {
        // Serve the link briefly first: the ack of the Sidecar's reply.
        const int64_t start = esp_timer_get_time();
        while (esp_timer_get_time() - start < 100000)
            sidecarHost.pollDiagnostic();
        LOG("Diagnostic mode: panic requested by the Sidecar\n");
        fflush(stdout);
        abort();
    }
    if (flags & static_cast<uint8_t>(SidecarDiagnosticFlag::FetchFirmware)) {
        sidecarHost.installFirmware();   // restarts into the new app; back here only on failure
        LOG("Diagnostic mode: OTA failed; serving the link until reset\n");
        fflush(stdout);
    }
    for (;;)
        sidecarHost.pollDiagnostic();   // serves Core (the core dump) too
}
#endif

#ifdef FAKE_SIO
// The bench (or csim) SIO stress test's fake 6502 (fakeSio.h), every
// FAKE_SIO_TICK_US from the bmon loop rather than from the 50 ms utility
// pass: roughly what the bmon loop sees under a real Atari's heavy PBI load,
// with core0_utilityTasks() still at its production rate. One step submits
// a request; then the dispatch a real 6502's trigger write ($D830/$D840)
// would get through core0_onBmonChange(), counted apart from the utility
// pass's catches (which should stay at 0); then another step completes it
// (or re-issues it for a copy-in, dispatched in the same tick too).
#ifndef FAKE_SIO_TICK_US
#define FAKE_SIO_TICK_US 1000
#endif
DRAM_ATTR int fakeSioTriggers = 0;
static inline __attribute__((always_inline)) IRAM_ATTR void core0_fakeSioTick() {
    CORE0_PHASE(PH_FAKESIO, 0);
    PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
    fakeSioPoll();
    bmonFakeWrBacklog = bmonFakeRdBacklog = true;   // (bmon.h)
    // A step that leaves a request pending (a new one, or a copy-in's
    // re-issue) gets its trigger at once, as a 6502's would: up to 4 rounds.
    for (int round = 0; round < 4; ++round) {
        // In order: whatever bmon holds goes first, as a trigger write
        // would come after it. Bounded, as in core0_utilityTasks().
        for (int n = 0; n <= bmonArraySzMask && bmonHead != bmonTail; ++n)
            core0_onBmonChange();
        if (pbiRequest[0].req != 0) {
            fakeSioTriggers++;
            handlePbiRequestHot(&pbiRequest[0]);
        } else if (pbiRequest[1].req != 0) {
            fakeSioTriggers++;
            handlePbiRequestHot(&pbiRequest[1]);
        } else {
            break;
        }
        fakeSioPoll();
        bmonFakeWrBacklog = bmonFakeRdBacklog = true;
    }
}
#endif

DRAM_ATTR bool everyNRanThisPass = false;   // EVERYN_TICKS (main.h)

static inline __attribute__((always_inline)) IRAM_ATTR bool core0_utilityTasks() {
    everyNRanThisPass = false;
    // The Sidecar link is polled here every 100 ms (below; branch
    // exp-txalign: the bmon loop serving Sidecar->main commands, which
    // until now only diagnostic mode did, sidecarDiagnosticMode()).
    // Main->Sidecar commands (SIO, CIO, Mount) are main's own calls.
    // A PBI request normally reaches handlePbiRequestHot() through bmon: the
    // 6502's write to the request block ($D830/$D840) is the one that sets
    // req. A pending req here means that dispatch never happened: the bmon
    // entry was lost, or a backlog discard (bmonTail = bmonHead) dropped it.
    // Every build polls both blocks on each pass (about every 50 ms) and
    // counts what it catches, which on real hardware should be nothing.
    // With FAKE_CLOCK (no 6502, so no trigger writes) this is the only way in.
    // Only with bmon drained: a trigger write still queued there gets its
    // normal dispatch (which then finds req clear, if this ran first). So
    // drain it here first: the work above (the Sidecar poll, ...) takes
    // longer than the longest write-free gap of a busy bus (FAKE_CLOCK's
    // fake bus: ~2.8 us), so without this the check almost never saw bmon
    // empty (11 of 590 passes) and the fallback never fired. Bounded, so a
    // write flood can't keep us here.
    for (int n = 0; n <= bmonArraySzMask && bmonHead != bmonTail; ++n)
        core0_onBmonChange();
    CORE0_PHASE(PH_UTIL, 0);
    if (bmonHead == bmonTail) {
        PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
        if (pbiRequest[0].req != 0) {
            pbiPollCatches++;
            handlePbiRequestHot(&pbiRequest[0]);
        } else if (pbiRequest[1].req != 0) {
            pbiPollCatches++;
            handlePbiRequestHot(&pbiRequest[1]);
        }
    }


    if (deferredInterrupt 
        && (d000Write[_0x1ff] & pbiDeviceNumMask) != pbiDeviceNumMask
        && (d000Write[_0x301] & 0x1) != 0
    )
        raiseInterrupt();

    if (/*XXINT*/1 && (ioCount > 0 ||
            (config.interruptStartDelay > 0 && elapsedSec > config.interruptStartDelay))) {
        static DRAM_ATTR uint32_t ltsc = 0;
        if (config.interruptTicks > 0 && XTHAL_GET_CCOUNT() - ltsc > config.interruptTicks) { 
            ltsc = XTHAL_GET_CCOUNT();
            raiseInterrupt();
        }
    }

#if defined(FAKE_CLOCK) 
// TODO: before we support FAKE io, avoid adding flakey data
// to the profile counters by copying them before enabling 
// core0 interrupts, and restoring the profile arrays after
// re-disabling interrupts, maybe in SCOPED_INTERRUPTS_ENABLE
    if (0 && elapsedSec > 10) { //XXFAKEIO
        // Stuff some fake PBI commands to exercise code in the core0 loop during timing tests 
        DRAM_ATTR static uint32_t lastTsc = XTHAL_GET_CCOUNT();
        DRAM_ATTR static constexpr uint32_t tickInterval = 240 * 1000;
        if (XTHAL_GET_CCOUNT() - lastTsc > tickInterval) {
            lastTsc = XTHAL_GET_CCOUNT();
            PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
            static int step = 0;
            if (step == 0) { 
                // stuff a fake CIO put request
                pbiRequest->cmd = 1; // interrupt 
                pbiRequest->stackprog = 0x82;
                pbiRequest->req = REQ_FLAG_STACKWAIT;
            } else if (step == 1) { 
                // stuff a fake SIO sector read request 
                AtariDCB *dcb = atariMem.dcb;
                dcb->DBUFHI = 0x40;
                dcb->DBUFLO = 0x00;
                dcb->DDEVIC = 0x31; 
                dcb->DUNIT = 2;
                dcb->DAUX1++; 
                dcb->DAUX2 = 0;
                dcb->DCOMND = 0x52;
                pbiRequest->cmd = 2; // read a sector 
                pbiRequest->stackprog = 0x82;
                pbiRequest->req = REQ_FLAG_STACKWAIT;
            } else if (step == 2) { 
                
            }
            step = (step + 1) % 2;
        }
    }
#endif 

    EVERYN_TICKS(240 * 1000010) { // XXSECOND
        CORE0_PHASE(PH_SEC, 0);
        elapsedSec++;
        if (config.diskIdleSwapDelay >= 0 && ioCount > 0 && elapsedSec > config.diskIdleSwapDelay + lastIoSec) { 
            lastIoSec = elapsedSec;
            if (--config.diskIdleSwapRepeat < 0) 
                config.diskIdleSwapDelay = -1;
            simulatedKeyInput.putKeys(config.diskIdleSwapKeys);
            diskRotatePending = true;   // rotateDrives(), at the next PBI request
        }


#if defined(TEST_PANIC) && TEST_PANIC > 0 && !defined(CSIM)
        // -DTEST_PANIC=<n>: crash core 0 here, hot, once elapsedSec passes
        // n, as a bug in normal operation would (a wild store:
        // StoreProhibited), to test the core dump path end to end: the
        // panic handler's dump, the reboot, the Sidecar's fetch. Once per
        // boot that didn't follow a panic (testPanicArmed), so main doesn't
        // crash-loop when nothing stops it after the reboot.
        if (testPanicArmed && elapsedSec > TEST_PANIC) {
            *reinterpret_cast<volatile uint32_t *>(testPanicAddr) = elapsedSec;
        }
#endif
#ifdef HAVE_REFRESH
        // refresh_ stuck low (writesRefreshLow): warn once, after enough
        // writes that a glitch can't be it.
        if (!refreshLowWarned && writesRefreshLow >= 1000) {
            refreshLowWarned = true;
            LOGHOT("WARNING: refresh_ (GPIO%d) read low on %u of %u writes: stuck low or miswired? "
                   "Core 1 records no reads; halts release blind (resume6502() timeouts %u)\n",
                   (int)bus.refresh_.pin, writesRefreshLow, bmonHead, resumeTimeouts);
        }
#endif
#ifndef CSIM
        {   // Core 1's watchdog (core1Fault, above).
            const unsigned heads = bmonHead + bmonRdHead;
            if (heads != core1WatchHeads) {
                core1WatchHeads = heads;
                core1QuietSec = 0;
                core1ClockStoppedLogged = false;
            } else if (++core1QuietSec >= kCore1QuietSec) {
                core1Stalled();
            }
        }
#endif
#if defined(TEST_CORE1_FAULT) && TEST_CORE1_FAULT > 0 && !defined(CSIM)
        // -DTEST_CORE1_FAULT=<n>: core 1 faults mid-run once elapsedSec
        // passes n (its L1 block pointers made wild for 1 ms: its next bus
        // cycle's load is a LoadProhibited), to test the watchdog's fault path.
        // Once per boot that didn't follow a panic. Not with FAKE_SIO: its
        // core 0 code reads the same pointers and can fault in that 1 ms.
        if (testCore1Armed && elapsedSec > TEST_CORE1_FAULT) {
            testCore1Armed = false;
            // Wild for 1 ms (core 1's next bus cycle is ~1 us away), then
            // back, so core 0's own later reads (the exit report's MMU dump)
            // find them intact.
            // Every table core 1 picks a block from: l1Blocks[], and for
            // $4000 and $C000 extBanks[] and osEnL1BlockMux[].
            static volatile uintptr_t wild = 4;   // volatile: the compiler can't see it's bad
            static L1Block *saved[nrL1Blocks + 256 + 4];
            L1Block **tables[] = {mmuState.l1Blocks, mmuState.extBanks, mmuState.osEnL1BlockMux};
            const int sizes[] = {nrL1Blocks, 256, 4};
            int n = 0;
            for (int t = 0; t < 3; ++t)
                for (int i = 0; i < sizes[t]; ++i) {
                    saved[n++] = tables[t][i];
                    tables[t][i] = reinterpret_cast<L1Block *>(wild);
                }
            const uint32_t t0 = XTHAL_GET_CCOUNT();
            while (XTHAL_GET_CCOUNT() - t0 < 240 * 1000) {}
            n = 0;
            for (int t = 0; t < 3; ++t)
                for (int i = 0; i < sizes[t]; ++i)
                    tables[t][i] = saved[n++];
        }
#endif
#if defined(TEST_CORE0_HANG) && TEST_CORE0_HANG > 0 && !defined(CSIM)
        // -DTEST_CORE0_HANG=<n>: core 0 hangs (spins, interrupts masked: no
        // watchdog, no reboot) once elapsedSec passes n, so main goes
        // silent on the link: the Sidecar's main link watchdog
        // (MAIN_LINK_SILENT, ioTimeoutSec). Its reset boots main in
        // diagnostic mode, where this never runs.
        if (elapsedSec > TEST_CORE0_HANG)
            for (;;) { asm volatile("" ::: "memory"); }
#endif
#if defined(TEST_CORE1_HANG) && TEST_CORE1_HANG > 0 && !defined(CSIM)
        // -DTEST_CORE1_HANG=<n>: core 1 stops (stalled, RUNSTALL) once
        // elapsedSec passes n, as a hang would: the watchdog's hang path.
        if (testCore1Armed && elapsedSec > TEST_CORE1_HANG) {
            testCore1Armed = false;
            REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
        }
#endif
#if defined(IRQ_JAM_TEST) && IRQ_JAM_TEST > 0
        if (irqJammed && elapsedSec >= IRQ_JAM_TEST) {
            irqUnjam(true);   // the bus is hot: core 1 releases the pin
            LOG("IRQ_JAM_TEST: irq_ released at %d s\n", elapsedSec);
        }
#endif
#if defined(TEST_FAIL_EXIT) && TEST_FAIL_EXIT > 0
        // -DTEST_FAIL_EXIT=<n>: a failure exit (negative, as the timeouts'
        // are) once elapsedSec passes n, to test the exit's core dump
        // (EXIT_COREDUMP) where nothing else fails: the timeouts are off
        // in FAKE_CLOCK builds.
        if (elapsedSec > TEST_FAIL_EXIT && exitFlag == 0)
            core0_exit(1, "-9 TEST_FAIL_EXIT");
#endif
        // At 15 s, once the boot's first I/O has come (DOS is up), or with
        // no drives at all (nothing to wait for: a BASIC or cartridge boot).
        if (elapsedSec == 15 && (ioCount > 0 || noDrivesConfigured())) {
            simulatedKeyInput.putKeys(config.bootKeyboardInput);
        }
#if CONFIG_A8BB_SYSMONITOR
        if (config.sysMonitorSec > 0 && (elapsedSec % config.sysMonitorSec) == 0) {  // XXSYSMON
            sysMonitorRequested = 1;
        }
#endif

#ifndef FAKE_CLOCK
        DRAM_ATTR static int lastWD = 0;
        if (watchDogCount == lastWD) { 
            secondsWithoutWD++;
        } else { 
            secondsWithoutWD = 0;
        }
        lastWD = watchDogCount;
#if 0 // XXPOSTDUMP
        if (sizeof(bmonTriggers) >= sizeof(BmonTrigger) && secondsWithoutWD == config.wdTimeoutSec - 1) {
            bmonTriggers[0].value = bmonTriggers[0].mask = 0;
            bmonTriggers[0].depth = 3000;
            bmonTriggers[0].count = 1;
    
        }
#endif
        if (config.wdTimeoutSec > 0 && secondsWithoutWD >= config.wdTimeoutSec) { 
            core0_exit(1, "-1 Watchdog timeout");
            return true;
        }
        if (config.ioTimeoutSec > 0 && elapsedSec - lastIoSec > config.ioTimeoutSec) {  
            core0_exit(1, "-2 IO timeout");
            return true;
        }
        if (1 && atariRam[_0x600] == 0xde) { 
            atariRam[_0x600] = 0;
            lastIoSec = elapsedSec;
            secondsWithoutWD = 0;
            ioCount++;
        }
#endif // #ifndef FAKE_CLOCK
        if (elapsedSec == 1) { 
            bmonMax = mmuChangeBmonMaxEnd = mmuChangeBmonMaxStart = 0;
            bmonStatsClear();
#ifdef FAKE_SIO
            bmonFakeWrHwm = bmonFakeRdHwm = 0;
#endif
        }
#if 0 // XXPOSTDUMP
        if (sizeof(bmonTriggers) >= sizeof(BmonTrigger) && elapsedSec == conf.runSec - 1) {
            bmonTriggers[0].value = bmonTriggers[0].mask = 0;
            bmonTriggers[0].depth = 1000;
            bmonTriggers[0].count = 1;
        }
#endif

        // config.runSec directly: a "static int runSec = config.runSec" here
        // got a C++ init guard (a flash call every second).
        const int runSec = config.runSec;
        if(elapsedSec > runSec && runSec > 0) {
            core0_exit(1, "0 Specified run time reached");
            return true;
        }
        if(atariRam[754] == 0xef || atariRam[764] == 0xef) {
            core0_exit(1, "1 Exit hotkey pressed");
            return true;
        }
        if(atariRam[754] == 0xee || atariRam[764] == 0xee) {
            config.wdTimeoutSec = config.ioTimeoutSec = 1200;
            lastIoSec = elapsedSec;
            secondsWithoutWD = 0;
            atariRam[712] = 255;
        }
        if(exitFlag) {
            if (exitReason.length() == 0) 
                core0_exit(exitFlag, "2 Exit command received");
            return true;
        }
    }
    // (EVERYN blocks from here on: longest interval first, main.h.)
    if (1) {
        static const DRAM_ATTR int keyTicks = 301 * 240 * 1000; // 150ms
        EVERYN_TICKS(keyTicks) {
            CORE0_PHASE(PH_KEY, 0); 
            // Inject the next key only once the Atari has taken the
            // last one (CH, $2FC, back to $FF), so none is overwritten.
            if (simulatedKeyInput.available() && atariRam[764] == 255) {
                uint8_t c = simulatedKeyInput.getKey();
                if (c != 255)  {
                    bmonMax = 0;
                    atariRam[764] = ascii2keypress[c];
                }
            }
        }
    }

#if A8BB_SIDECAR
    // Serve the Sidecar from the bmon loop, every 100 ms: poll() is one
    // i2slink_hint() when nothing is new; when it is, it reads the frame
    // (waiting at most one beam window, ~374 us) and answers one command
    // (Nop, the host's object reads; hot-safe: IRAM, memory only). Its
    // time counts against the bmon keep-up rule (bmon.h): the two rings
    // mean only reads can be lost while it runs, never a write. The
    // slowest poll goes into the io: line ("poll"). Last of the EVERYN
    // blocks: the shortest interval goes last (main.h), or it starves them.
    // -DNO_LINK_POLL=1 (bench A/B): leave it out.
#ifndef NO_LINK_POLL
    EVERYN_TICKS(240 * 100000) {
        CORE0_PHASE(PH_POLL, 0);
        const uint32_t t0 = XTHAL_GET_CCOUNT();
        if (sidecarHost.poll())
            sidecarHost.pollsServed++;
        // The Sidecar's one-shot flags (SetFlags, SidecarMainFlag), acted on
        // here, after the poll that took them. Interrupt: the PBI device's
        // IRQ, as config.interruptTicks raises it (deferred by
        // raiseInterrupt() itself while the device isn't selectable).
        if (sidecarHost.sidecar().takeMainFlags(static_cast<uint32_t>(SidecarMainFlag::Interrupt)))
            raiseInterrupt();
        // Screenshot: sent from the cold PBI handler's next IRQ call (pbi.cpp).
        if (sidecarHost.sidecar().takeMainFlags(static_cast<uint32_t>(SidecarMainFlag::Screenshot)))
            screenshotRequested = 1;
        // Reboot: park the 6502, then reset (requestSafeReboot()).
        if (sidecarHost.sidecar().takeMainFlags(static_cast<uint32_t>(SidecarMainFlag::Reboot)))
            requestSafeReboot();
        const uint32_t dt = XTHAL_GET_CCOUNT() - t0;
        sidecarHost.polls++;
        if (dt > sidecarHost.pollTicksMax)
            sidecarHost.pollTicksMax = dt;
        const uint32_t us = dt / 240U;
        ++sidecarHost.pollUs[us < 4 ? 0 : us < 16 ? 1 : us < 64 ? 2 : us < 256 ? 3 : 4];
    }
#endif   // NO_LINK_POLL
#endif

    return false;
}

// heap_caps_print_heap_info() through LOG(): IDF's prints with printf(),
// which only reaches main's own console, so -sc captures (the Sidecar's
// console) lost the startup and shutdown heap reports. The same function,
// line for line, on IDF's private region list (heap_private.h, from the
// heap component's directory: main/CMakeLists.txt): each region's
// multi_heap_get_info() takes and drops its lock, and LOG() runs outside
// it. Flash code, cold bus only, like IDF's.
#ifdef CSIM
static void logHeapInfo(uint32_t) {}   // csim has no ESP heap
#else
static void logHeapInfo(uint32_t caps) {
    multi_heap_info_t info;
    LOG("Heap summary for capabilities 0x%08" PRIX32 ":\n", caps);
    heap_t *heap;
    SLIST_FOREACH(heap, &registered_heaps, next) {
        if (heap_caps_match(heap, caps)) {
            multi_heap_get_info(heap->heap, &info);
            LOG("  At 0x%08x len %d free %d allocated %d min_free %d\n", (unsigned)heap->start,
                (int)(heap->end - heap->start), (int)info.total_free_bytes, (int)info.total_allocated_bytes,
                (int)info.minimum_free_bytes);
            LOG("    largest_free_block %d alloc_blocks %d free_blocks %d total_blocks %d\n",
                (int)info.largest_free_block, (int)info.allocated_blocks, (int)info.free_blocks,
                (int)info.total_blocks);
        }
    }
    LOG("  Totals:\n");
    heap_caps_get_info(&info, caps);
    LOG("    free %d allocated %d min_free %d largest_free_block %d\n", (int)info.total_free_bytes,
        (int)info.total_allocated_bytes, (int)info.minimum_free_bytes, (int)info.largest_free_block);
}
#endif

volatile float foo = 1.01;
CORE0_HOTSPOTS_ATTR void core0Loop() {
    psramPtr = psram;

    // disable PBI ROM by corrupting it 
    //pbiROM[0x03] = 0xff;

    int bmonCaptureDepth = 0;

    const static DRAM_ATTR int prerollBufferSize = 64; // must be power of 2
    uint32_t prerollBuffer[prerollBufferSize]; 
    uint32_t prerollIndex = 0;

    if (psram == NULL) {
        for(auto &t : bmonTriggers) t.count = 0;
    }
    busyWait6502Ticks(10000);
    // TODO: why is this needed?  seems to hint at a bug in core1 loop maybe impacting resume6502 
    // elsewhere.  Possibly figured out, see notes in resume6502()
    //REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.halt_.mask);
    // Not resume6502(): its blind wait can end before core 1 has served a
    // cycle here, and the 6502 never boots (it waited 15 bmon entries
    // when bmon logged reads too). Hold the halt release until the 6502 has
    // written twice; bmon logs only writes now. Wait as long as the Atari
    // takes to power up and run: a short timeout here took the release
    // back before core 1 had served a cycle on miner6 #2, whose Atari
    // comes up later, and the 6502 stayed halted. The wait gives up after
    // 1 s with a bench FAKE_CLOCK and 30 s on real hardware, and the run
    // ends there (exit -6). Only time with the bus clock running counts:
    // an Atari still off (phi2 stopped) is waited for, not a failure. A
    // main that booted while its Sidecar was being flashed (boot.sh -sc,
    // the Atari off; ~40 s on miner6 .4) timed out here every run, and
    // its core dump was the next run's "main crashed" (2026-11-25). The
    // clock test is memory only (core 1 is on the bus: no GPIO reads from
    // core 0): core 1 pushes every bus cycle but refresh into one bmon ring
    // or the other, the 6502 halted or not, so the heads move exactly when
    // phi2 runs; each 1 ms window counts if they moved in it.
    bmonTail = bmonHead;
    mmuState.pinReleaseMask |= bus.halt_.mask;
#if !defined(CSIM) || defined(CSIM_BUS_CYCLE)   // csim: core 0 is a coroutine there
    {
#ifdef FAKE_CLOCK
        static const DRAM_ATTR uint64_t firstWriteTimeout = 240000000ULL;       // 1 s
#else
        static const DRAM_ATTR uint64_t firstWriteTimeout = 30 * 240000000ULL;  // 30 s
#endif
        static constexpr uint32_t kWindow = 240000;   // 1 ms
        uint32_t windowStart = XTHAL_GET_CCOUNT();
        unsigned windowHeads = bmonHead + bmonRdHead;
        uint64_t waited = 0;   // with the clock running; CCOUNT wraps every 17.9 s
        for (int n = 0; n < 2; n++) {
            const unsigned int oldHead = bmonHead;
            while (oldHead == bmonHead && waited < firstWriteTimeout) {
                AsmNops<30>::generate();
                const uint32_t now = XTHAL_GET_CCOUNT();
                if (now - windowStart >= kWindow) {
                    const unsigned heads = bmonHead + bmonRdHead;
                    if (heads != windowHeads)   // bus cycles: phi2 runs
                        waited += now - windowStart;
                    windowHeads = heads;
                    windowStart = now;
                }
            }
        }
        // Fatal: return through threadFunc's exit path, which stalls core 1
        // and unmasks interrupts before printing. No printf here: with
        // interrupts masked, newlib's stdout lock calls abort().
        if (waited >= firstWriteTimeout) {
            core0_exit(1, "-6 Startup timeout, no 6502 writes seen");
            return;
        }
    }
#endif
    busyWait6502Ticks(15);
    mmuState.pinReleaseMask &= haltMaskNOT;

#if defined(PROFILE_GPIO_SAMPLE) && defined(FAKE_CLOCK)
    profileGpioSampleBurstForCycles(PROFILE_GPIO_SAMPLE_CYCLES);
    core0_exit(1, "0 Specified sample time reached");
#endif

    bmonTail = bmonHead;
#ifdef FAKE_SIO
    // Short bmon passes for the fake 6502 (core0_fakeSioTick()); the utility
    // tasks still every ~50 ms, every fakeSioUtilityEvery-th pass.
    constexpr int fakeSioUtilityEvery = 50 * 1000 / FAKE_SIO_TICK_US;
    int fakeSioPasses = 0;
#endif
    while(!exitFlag) {
        uint32_t stsc = XTHAL_GET_CCOUNT();
#ifdef FAKE_SIO
        const static DRAM_ATTR uint32_t bmonTimeout = 240 * FAKE_SIO_TICK_US;
#else
        const static DRAM_ATTR uint32_t bmonTimeout = 240 * 1000 * 50;
#endif
	    while(XTHAL_GET_CCOUNT() - stsc < bmonTimeout) {
            CSIM_HOOK();
#if !defined(CSIM) || defined(CSIM_BUS_CYCLE)
            // Warning: this is a core0 ifetch hotspot, with a hand-tuned loop delay.
            // Either ring wakes it (reads arrive on nearly every cycle;
            // the rings are drained alternately: a write, then a few reads).
            while(
                XTHAL_GET_CCOUNT() - stsc < bmonTimeout && 
                bmonHead == bmonTail && bmonRdHead == bmonRdTail) {
#pragma GCC unroll 8
                // (Writes only here: unrolled 8 times, every check costs IRAM.)
                for (int poll = 0; poll < 8 && bmonHead == bmonTail; ++poll) {
                    loopNezDelay(33);   // 30 passes + setup; was 15 two-cycle passes
                }
                // csim: nothing to do until the 6502's next bus cycle, so let
                // it run now instead of spinning out the clock-read budget.
                CSIM_CORE0_YIELD();
            }
#endif // CS
            // Alternate: a write, then a few reads (bmonRdBatch), so a
            // write backlog can't hold R-Time 8's $D5B9 reads (the
            // time-critical ones: each must be answered before the next,
            // ~4 bus cycles on) behind it; the writes' work tolerates a
            // few us more (I/O and resume latency).
            if (bmonHead != bmonTail)
                core0_onBmonChange();
            if (bmonRdHead != bmonRdTail)
                core0_onBmonReads();
        }
        //restartHalted6502();
#ifdef FAKE_SIO
        core0_fakeSioTick();
        if (++fakeSioPasses < fakeSioUtilityEvery)
            continue;
        fakeSioPasses = 0;
#endif
        if (core0_utilityTasks())
            break;
    }
}

#if !A8BB_SIDECAR && !defined(GPIO19_IN_USE) && !defined(CSIM)
static void waitForUsbConsoleHost(const char *when, uint32_t timeoutMs);
#endif

void IFLASH_ATTR threadFunc(void *) { 
    LOG("CORE0: threadFunc() start\n");
#if defined(SLED_TEST) && SLED_TEST
    sledLanding();
#endif

    LOG("runSec %d\n", config.runSec);
    uint8_t chipid[6];
    esp_read_mac(chipid, ESP_MAC_WIFI_STA);
    mmuDebugPrint();
    LOG("HW_TYPE %s\n", HW_TYPE);
    LOG("BOARD %s\n", A8BB_BOARD_NAME);   // the pin map (Kconfig A8BB_MAIN_BOARD, pinDefs.h)
    LOG("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",chipid[0], chipid[1], chipid[2], chipid[3], chipid[4], chipid[5]);
    LOG("GIT: " GIT_VERSION " \n");
#ifdef BOOT_TOKEN
    // -DBOOT_TOKEN=<n>: a per-build marker, so a test (boot.sh -sc's
    // reflash soak) can tell this build's image from the last one's.
    LOG("BOOT_TOKEN %d\n", (int)(BOOT_TOKEN));
#endif
    // Setup is done and core 1 has the bus: boot.sh powers on the Atari at
    // this line (tools/power_on_when_up.sh). Keep the text as it is. The
    // Sidecar, told so, releases the Atari's RESET, which it held while
    // main booted.
    LOG("boot config: %s\n", bootConfigSummary.c_str());
#if defined(SLED_TEST) && SLED_TEST
    LOG("SLED_TEST: BOOT COMPLETE was early (the Atari powered on before the sled)\n");
#else
    LOG("BOOT COMPLETE\n");
    sidecarSystemFlags(static_cast<uint32_t>(SidecarSystemFlag::BootComplete), 0);
#endif

    //XT_INTEXC_HOOK oldnmi = _xt_intexc_hooks[XCHAL_NMILEVEL];
    uint32_t oldint;

    portDISABLE_INTERRUPTS();
    disableCore0WDT();
    //_xt_intexc_hooks[XCHAL_NMILEVEL] = my_nmi; 
    //__asm__ __volatile__("rsil %0, 1" : "=r"(oldint) : );

    core0Loop();
#ifndef FAKE_CLOCK
    // Both rings (writes, then reads): the reads show a wedged 6502's loop.
    BmonEntry *bmonCopy = new BmonEntry[bmonArraySz + bmonRdArraySz];
    for(int i = 0; i < bmonArraySz; i++)
        bmonCopy[i].r0 = bmonArray[i].r0;
    for(int i = 0; i < bmonRdArraySz; i++)
        bmonCopy[bmonArraySz + i].r0 = bmonRdArray[i].r0;
#endif
    //busywait(.5);
    //disableBus();
    //busywait(.001);
    REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
    uint32_t in0 = REG_READ(GPIO_IN_REG);
    uint32_t in1 = REG_READ(GPIO_IN1_REG);
    uint32_t en0 = REG_READ(GPIO_ENABLE_REG);
    uint32_t en1 = REG_READ(GPIO_ENABLE1_REG);

    busywait(.001);
    enableCore0WDT();
    portENABLE_INTERRUPTS();
#if !A8BB_SIDECAR && !defined(GPIO19_IN_USE) && !defined(CSIM)
    waitForUsbConsoleHost("exit report", 2000);
#endif
#ifdef FAKE_CLOCK
    LOG("Shutdown atariRam[0x0180]=%02x\n", atariRam[0x0180]);
#endif
    //_xt_intexc_hooks[XCHAL_NMILEVEL] = oldnmi;
    //__asm__("wsr %0,PS" : : "r"(oldint));
    LOG("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", in0, in1);
    LOG("GPIO_EN_REG: %08" PRIx32 " %08" PRIx32 "\n", en0, en1);
    LOG("in1: ");
    if ((in1 & bus.extSel.mask) == 0) LOG("(extsel_ AL)");
    if ((in1 & bus.halt_.mask) == 0) LOG("(halt_ AL)");
    if ((in1 & bus.irq_.mask) == 0) LOG("(irq_ AL)");
    if ((in1 & bus.mpd.mask) == 0) LOG("(mpd_ AL)");
    LOG("\n");
    LOG("en1: ");
    if ((en1 & bus.extSel.mask) != 0) LOG("(extsel_)");
    if ((en1 & bus.halt_.mask) != 0) LOG("(halt_)");
    if ((en1 & bus.irq_.mask) != 0) LOG("(irq_)");
    if ((en1 & bus.mpd.mask) != 0) LOG("(mpd_)");
    LOG("\n");
    logHeapInfo(MALLOC_CAP_SPIRAM);
    logHeapInfo(MALLOC_CAP_INTERNAL);
    mmuDebugPrint();
#ifndef FAKE_CLOCK
    LOG("bmonMax: %d mmuChangeBmonMaxEnd: %d mmuChangeBmonMaxStart: %d laps: writes %u reads %u\n", bmonMax,
        mmuChangeBmonMaxEnd, mmuChangeBmonMaxStart, bmonLaps, bmonRdLaps);   
    LOG("bmonArray:\n");
    uint16_t *addrHistogram = (uint16_t *)(uint8_t *)heap_caps_malloc(64 * 1024 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    LOG("addrHistogram allocated %p\n", addrHistogram);
    if (addrHistogram != NULL) { 
        for(int n = 0; n < 64 * 1024; n++) addrHistogram[n] = 0;

        for(int i = 0; i < (int)(bmonArraySz + bmonRdArraySz); i++) { 
            uint32_t r0 = (bmonCopy[i].r0) >> bmonR0Shift;
            uint16_t addr = r0ToAddr(r0);
            addrHistogram[addr]++;
 	
            uint8_t data = (bmonCopy[i].r0 & 0xff);
            if (bmonExclude(bmonCopy[i].r0)) continue;
            if (0) { // exhaustively print every bmon entry  
                char rw = (r0 & bus.rw.mask) != 0 ? 'R' : 'W';
#ifdef HAVE_REFRESH
                if ((r0 & bus.refresh_.mask) == 0) rw = 'F';
#endif
                    LOG("%c %04x %02x\n", rw, addr, data);
            }
        }
        for(int addrCount = 0; addrCount < 5; addrCount++) {
            uint16_t hotAddr = 0;
            int hotAddrCount = 0;
            for(int n = 0; n < 64 * 1024; n++) {
                if (addrHistogram[n] > hotAddrCount) {
                    hotAddr = n;
                    hotAddrCount = addrHistogram[n];
                }
            }
            LOG("bmon hotspot %d: addr=%04x count=%d\n", addrCount, (int)hotAddr, hotAddrCount);
            addrHistogram[hotAddr] = 0;
       }
    }
 #endif


#ifdef PROFILE_ENABLE_ARRAY
    profilePrint(opt.histogram, ramReads);
#endif
    
    LOG("DUMP %.2f\n", millis() / 1000.0);
    
    if (opt.tcpSendPsram && psram != NULL) { 
        LOG("TCP SEND %.2f\n", millis() / 1000.0);
        //j.begin();
        //wdtReset();
        yield();
        //disableCore0WDT();
        //disableLoopWDT();
        while(!sendPsramTcp((char *)psram, psram_sz)) delay(1000);
    }
#ifndef FAKE_CLOCK
    if (opt.dumpPsram && psram != NULL) {
        const char *ops[256] = {0}; // XXOPS
        //ops[0] = "brk";
        ops[0x60] = "rts";
        ops[0xd8] = "cld";
        ops[0x68] = "pla";
        ops[0x85] = "sta $xx";
        ops[0x95] = "sta $xx,x";
        ops[0x8d] = "sta $xxxx";
        ops[0x9d] = "sta $xxxx,x";
        ops[0x81] = "sta ($xx,x)";
        ops[0x91] = "sta ($xx,y)";
        ops[0x95] = "sta $xxxx,y";

        ops[0xa9] = "lda #nn";
        ops[0xa5] = "lda $nn";
        ops[0xad] = "lda $nnnn";

        ops[0x40] = "rti";
        ops[0x4c] = "jmp $nnnn";

        ops[0xaa] = "tax";
        ops[0xa8] = "tay";
        ops[0x98] = "tya";
        ops[0xba] = "tsx";
        ops[0x9a] = "txs";

        ops[0xa0] = "ldy #nn";

        ops[0x8e] = "stx $nnnn";
        ops[0x86] = "stx $nn";
        ops[0x96] = "stx $nn,y";

        ops[0x8c] = "sty $nnnn";
        ops[0x84] = "sty $nn";
        ops[0x94] = "sty $nn,x";
        ops[0x58] = "cli";
        ops[0x78] = "sei";

        ops[0xf0] = "beq $nn";
        ops[0xd0] = "bne $nn";
        ops[0x30] = "bmi $nn";
        ops[0x10] = "bpl $nn";
        ops[0x90] = "bcc $nn";
        ops[0xb0] = "bcs $nn";

        ops[0x20] = "jsr $aaaa";
        ops[0xa2] = "ldx #nn";
        ops[0x24] = "bit $nn";
        ops[0x2c] = "bit $nnnn";

        ops[0xce] = "dec $nnnn";
        ops[0xde] = "dec $nn,x";
        ops[0xee] = "inc $nnnn";
        ops[0xfe] = "inc $nn,x";

        ops[0x6c] = "jmp ($nnnn)";
        ops[0xe6] = "inc $nn";
        ops[0xcc] = "cpy $nnnn";
        ops[0x0e] = "asl #nn";
        ops[0x08] = "php";
        ops[0xc0] = "cpy #nn";
        ops[0xc8] = "iny";
        ops[0xb9] = "lda $nnnn,y";
        ops[0x38] = "sec";
        ops[0x18] = "clc";


        uint32_t lastTrigger = 0;

        for(uint32_t *p = psram; p < psram + min(opt.dumpPsram, (int)(psram_end - psram)); p++) {
            //printf("P %08X\n",*p);
            //if ((*p & copyResetMask) && !(*p &pins.extDecode.mask))
            //if ((*p & copyResetMask) != 0)
            //s += sfmt("%08x\n", *p);

            if (1) {
                if ((*p & 0x80000000) != 0 && p < psram_end - 1) {
                    LOG("BT%7d us ", (int)(*(p + 1) - lastTrigger) / 240);
                    lastTrigger = *(p + 1);
                } else if (*p != 0) { 
                    LOG("B            ");
                }
                uint32_t r0 = ((*p) >> bmonR0Shift);
                uint16_t addr = r0ToAddr(r0);
                char rw = (r0 & bus.rw.mask) != 0 ? 'R' : 'W';
#ifdef HAVE_REFRESH
                if ((r0 & bus.refresh_.mask) != 0) rw = 'F';
#endif
                uint8_t data = (*p & 0xff);
                const char *op = ops[data];
                if (op == NULL) op = "";
                if (*p != 0) {
                    LOG("%c %04x %02x   %s\n", rw, addr, data, op);
                }
                if ((*p & 0x80000000) != 0 && p < psram_end - 1) {
                    // skip the timestamp
                    p++;
                }
            }
            //if (p > psram + 1000) break;
            //wdtReset();
            //if (((p - psram) % 0x1000) == 0) printf("%08x\n", p - psram);
        }
        yield();
    }
    LOG("atariRam[754] = %d\n", atariRam[754]);
    LOG("pbiROM[0x100] = %d\n", pbiROM[0x100]);
    LOG("reg[0xd301] = 0x%02x\n", d000Write[0x301]);
    LOG("reg[0xd1ff] = 0x%02x\n", d000Write[0x1ff]);
    LOG("ioCount %d, interruptCount %d\n", ioCount, pbiInterruptCount);
    if (structLogs) structLogs->print();
    LOG("Page 6: ");
    for(int i = 0x600; i < 0x620; i++) { 
        LOG("%02x ", atariRam[i]);
    }
    LOG("\nHATABS: ");
    for(int x = 0x031A; x <= 0x31a + 36 && atariRam[x] != 0; x += 3) { 
        LOG("%c=%04x ", atariRam[x], atariRam[x + 1] + (atariRam[x + 2] << 8));
    }
    LOG("\npbiROM:\n");
    for(int i = 0; i < min((int)sizeof(pbiROM), 0x60); i++) { 
        LOG("%02x ", pbiROM[i]);
        if (i % 16 == 15) LOG("\n");
    }
    LOG("\nstack:\n");
    for(int i = 0; i < 256; i++) { 
        if (i % 16 == 0) LOG("\n0x1%02x: ", i);
        LOG("%02x ", atariRam[i + 0x100]);
    }
    LOG("\ndisplay list:\n");
    uint16_t dlist = atariRam[560] + (atariRam[561] << 8);
    for(int i = 0; i < 32; i++) { 
        if (i % 8 == 0) LOG("%04x: ", dlist + i);
        LOG("%02x ", atariRam[dlist + i]);
        if (i % 8 == 7) LOG("\n");
    }
    dumpScreenToSerial('B');
    dumpScreenshot("shutdown");   // graphics modes too: tools/screenshot_png.py
#endif
    
    int memReadErrors = 0;//(atariRam[0x609] << 24) + (atariRam[0x608] << 16) + (atariRam[0x607] << 16) + atariRam[0x606];
    LOG("SUMMARY %-10.2f/%d e%d i%d d%d %s\n", millis()/1000.0, config.runSec, memReadErrors, 
        pbiInterruptCount, ioCount, exitReason.c_str());
    LOG("PBI requests caught by the poll, not bmon: %d\n", pbiPollCatches);
#ifdef FAKE_SIO
    LOG("FAKE_SIO: %d requests dispatched by the fake trigger (core0_fakeSioTick())\n", fakeSioTriggers);
    fakeSioReport();
#endif
    LOG("pbi_init_complete %d, halts %d, resume6502() timeouts %u\n", pbiROM[0x20], haltCount, resumeTimeouts);
#ifdef HAVE_REFRESH
    LOG("writes with refresh_ low: %u of %u%s\n", writesRefreshLow, bmonHead,
        writesRefreshLow ? " (refresh_ stuck low or miswired?)" : "");
#endif
    LOG("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    LOG("GPIO_EN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_ENABLE_REG),REG_READ(GPIO_ENABLE1_REG)); 
    LOG("extMem swaps %d evictions %d d1ff %02x pinDr %08lx\n", 
        extMem.swapCount, extMem.evictCount, d000Write[0x1ff], 0L/*pinDriveMask*/);

    LOG("cartBankLatch %02x atariCart.bankA0 %02x l1Blocks[0xa0] %p cartBanks[1] %p atariCart.image[1].l1Block %p\n",
        cartBankLatch, atariCart.bankA0, mmuState.l1Blocks[l1BlockA0], mmuState.cartBanks[1], atariCart.image != NULL ? &atariCart.image[1].l1Block : 0);
    LOG("interruptTicks=%d,interruptRequested=%d,deferredInterrupt=%d\n", config.interruptTicks, interruptRequested, deferredInterrupt);
    LOG("STACK HIGH-WATER: minimum unused / configured stack (bytes)\n");
    auto printStackHighWater = [](const char *name, TaskHandle_t task, size_t stackSizeBytes) {
        if (task == 0) {
            LOG("STACK %-16s not running\n", name);
            return;
        }
        UBaseType_t freeBytes = uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t);
        LOG("STACK %-16s %u/%u bytes free\n", name, (unsigned)freeBytes,
            (unsigned)stackSizeBytes);
    };
    auto printNamedTaskStackHighWater = [&printStackHighWater](const char *name, size_t stackSizeBytes) {
        printStackHighWater(name, xTaskGetHandle(name), stackSizeBytes);
    };
    // core0Loop() runs on the main task (setup() calls threadFunc() directly;
    // its own task is gone), so one stack: ESP_TASK_MAIN_STACK.
    printStackHighWater("main (core0Loop)", xTaskGetCurrentTaskHandle(), ESP_TASK_MAIN_STACK);
    printNamedTaskStackHighWater("sys_evt", ESP_TASKD_EVENT_STACK);
#if CONFIG_COMPILER_OPTIMIZATION_NONE
    constexpr size_t ipcStackSizeBytes = CONFIG_ESP_IPC_TASK_STACK_SIZE + 0x100;
#else
    constexpr size_t ipcStackSizeBytes = CONFIG_ESP_IPC_TASK_STACK_SIZE;
#endif
    printNamedTaskStackHighWater("ipc0", ipcStackSizeBytes);
    printNamedTaskStackHighWater("ipc1", ipcStackSizeBytes);
    printNamedTaskStackHighWater("esp_timer", ESP_TASK_TIMER_STACK);
    printNamedTaskStackHighWater(CONFIG_FREERTOS_TIMER_SERVICE_TASK_NAME,
        CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH);
    printNamedTaskStackHighWater("tcpip_thread", ESP_TASK_TCPIP_STACK);
    printNamedTaskStackHighWater(CONFIG_ESP32_PTHREAD_TASK_NAME_DEFAULT,
        CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT);
    sidecarSampling(SidecarSampleAction::Stop, 0);   // the run is over: the Sidecar reports the last period
    a8logReport();
    // A failure exit (its reason starts with a negative number: "-2 IO
    // timeout", "-3 stackprog timeout" ...): panic after the report, so the
    // run leaves a core dump of the state it failed in (atariRam, the maps,
    // the bmon rings). DONE still comes first, unchanged, for the tools that
    // read it; the capture scripts read on after a negative DONE (boot.sh:
    // to the panic's "Rebooting", for crash_bundle.sh; -sc: to the
    // Sidecar's fetch of the core). The pause lets the capture reopen the
    // port. Test: -DTEST_FAIL_EXIT=<n>.
    const bool failExit = exitReason.size() > 1 && exitReason[0] == '-'
                          && isdigit((unsigned char)exitReason[1]);
#ifndef CSIM
    if (failExit)
        LOG("EXIT_COREDUMP: a failure exit; panicking after DONE, for a core dump\n");
#endif
    LOG("DONE %-10.2f %s\n", millis() / 1000.0, exitReason.c_str());
    a8logFlush();
    delay(100);
#ifndef CSIM
    if (failExit) {
        delay(1000);
        esp_system_abort(exitReason.c_str());
    }
#else
    (void)failExit;
#endif
    
    //ESP.restart();
#ifndef CSIM
    LOG("CORE0 idle\n");
    while(1) { 
        //printf("CORE0 idle\n");
        delay(10); 
        yield();
    }
#endif
}

// Core 1's stack, static so it sits at a fixed place in internal SRAM
// (.bss) rather than wherever the heap puts it, possibly beside data core 0
// hammers. app_cpu_init() loads app_cpu_stack_ptr into a1 as the initial
// stack pointer: Xtensa stacks grow down, so that is the array's top.
alignas(16) uint8_t app_cpu_stack[4096];
void *app_cpu_stack_ptr = app_cpu_stack + sizeof(app_cpu_stack);
static void IFLASH_ATTR app_cpu_main();
static void IFLASH_ATTR app_cpu_init()
{
    // Reset the reg window. This will shift the A* registers around,
    // so we must do this in a separate ASM block.
    // Otherwise the addresses for the stack pointer and main function will be invalid.
    ASM(                                \
        "movi a0, 0\n"                            \
        "wsr  a0, WindowStart\n"                \
        "movi a0, 0\n"                            \
        "wsr  a0, WindowBase\n"                    \
        );
    // init the stack pointer and jump to main function
    ASM("l32i a1, %0, 0\n"::"r"(&app_cpu_stack_ptr));
    ASM("callx4   %0\n"::"r"(app_cpu_main));
    REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
}

// Keep the heap out of the DRAM the ROM uses when startCpu1() boots core 1.
// In single-core builds IDF recycles the ROM's reserved DRAM into the heap
// ("recycled by heap allocator in app_main task", heap/port/esp32s3/
// memory_layout.c), assuming the ROM is done with it. But startCpu1() resets
// core 1 through the ROM, which then runs on its APP CPU stack (.stack_app)
// and re-initializes its own data (.bss_ets ... .bss_xtos), writing over any
// heap objects placed there. Seen as a FreeRTOS assert in
// xTaskRemoveFromEventList: the partition-list mutex sat at 0x3fcee360, and
// its xTasksWaitingToSend header, at the ROM's _Pri_4/_Pri_5_HandlerAddress
// (0x3fcee370), held their ROM defaults _Level4FromVector/_Level5FromVector.
// Layout from the ROM ELF (esp32s3_rev0_rom.elf): .stack_app starts at
// 0x3fceb710; .bss_xtos ends at 0x3fceee34, where IDF's own reservation for
// the ROM's USB/UART data begins.
//
// Most of the span is needed only while the ROM boots core 1: once core 1 is
// in app_cpu_main() it runs our code on our own stack with interrupts masked.
// So setup() reclaims the span after that (reclaimRomAppCpuBootDram()), all
// but .data_xtos: core 1 keeps the ROM's vectors, which dispatch a core 1
// exception through the tables there (xtos_exc_handler_table and friends;
// the ROM's core 1 boot also writes _Pri_4/_Pri_5_HandlerAddress there).
// startCpu1() runs once per boot, and a panic stops core 1 rather than
// restarting it, so the ROM never boots core 1 again over the heap.
//
// This rests on measurements of the current ROM and IDF (AGENTS.md, "ROM
// core 1 boot DRAM", has them and how to repeat them), so the reclaim checks
// them on every boot: the parts are filled with a pattern before startCpu1()
// and are reclaimed only if the pattern survives core 1's boot.
#if !defined(CSIM) && CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
#define ROM_APP_CPU_BOOT_START 0x3fceb710
#define ROM_APP_CPU_BOOT_END 0x3fceee34
#define ROM_XTOS_DATA_START 0x3fcedf60   // .data_xtos: exception/interrupt tables
#define ROM_XTOS_DATA_END 0x3fcee378
SOC_RESERVE_MEMORY_REGION(ROM_APP_CPU_BOOT_START, ROM_APP_CPU_BOOT_END, rom_app_cpu_boot);
#endif

// Set by core 1 once it is out of the ROM and running app_cpu_main().
DRAM_ATTR volatile bool appCpuRunning = false;

#if !defined(CSIM) && CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
// The span minus .data_xtos: the part the heap gets back.
static const struct { uintptr_t start, end; } kRomReclaimParts[] = {
    {ROM_APP_CPU_BOOT_START, ROM_XTOS_DATA_START},
    {ROM_XTOS_DATA_END, ROM_APP_CPU_BOOT_END},
};
static constexpr uint8_t kRomReclaimPattern = 0xa5;

// Fill the parts to reclaim, reserved and unused until then, so a later
// check shows whether anything wrote them.
static void romReclaimFill(uint8_t pattern) {
    for (const auto &part : kRomReclaimParts) {
        memset((void *)part.start, pattern, part.end - part.start);
    }
}

// Count the bytes of [start, end) that differ from expected[] (or, without
// it, from pattern), printing each changed stretch (changes up to 16 bytes
// apart merged).
static size_t romSpanChanges(const char *when, uintptr_t start, uintptr_t end, uint8_t pattern,
                             const uint8_t *expected = nullptr) {
    const uint8_t *span = (const uint8_t *)start;
    const size_t length = end - start;
    auto same = [&](size_t i) { return span[i] == (expected ? expected[i] : pattern); };
    size_t changed = 0;
    for (size_t i = 0; i < length;) {
        if (same(i)) { ++i; continue; }
        size_t last = i + 1, gap = 0;
        for (size_t j = i; j < length && gap < 16; ++j) {
            if (!same(j)) { last = j + 1; gap = 0; } else { ++gap; }
        }
        size_t count = 0;
        for (size_t j = i; j < last; ++j) count += !same(j);
        LOG("  %s: %08x..%08x changed (%u of %u bytes)\n", when, (unsigned)(start + i),
               (unsigned)(start + last), (unsigned)count, (unsigned)(last - i));
        changed += count;
        i = last;
    }
    return changed;
}

static size_t romReclaimChanges(const char *when, uint8_t pattern) {
    size_t changed = 0;
    for (const auto &part : kRomReclaimParts) {
        changed += romSpanChanges(when, part.start, part.end, pattern);
    }
    return changed;
}

// Give the span, minus .data_xtos, to the heap once core 1 is out of the
// ROM, and only if the ROM's core 1 boot left those parts untouched
// (romReclaimFill(kRomReclaimPattern) ran before startCpu1()). A ROM or IDF
// change that starts using them shows up here as a refusal, not as heap
// corruption. Returns the bytes added.
static size_t reclaimRomAppCpuBootDram() {
    const uint32_t start = XTHAL_GET_CCOUNT();
    while (!appCpuRunning) {
        if (XTHAL_GET_CCOUNT() - start > 240u * 1000u * 1000u) {
            LOG("ROM DRAM reclaim: core 1 not up after 1 s; keeping the span reserved\n");
            return 0;
        }
    }
    const size_t changed = romReclaimChanges("ROM core 1 boot", kRomReclaimPattern);
    if (changed) {
        LOG("ROM DRAM reclaim: the ROM's core 1 boot wrote %u bytes outside .data_xtos; "
               "keeping the span reserved (see AGENTS.md)\n", (unsigned)changed);
        return 0;
    }
    size_t added = 0;
    for (const auto &part : kRomReclaimParts) {
        if (heap_caps_add_region(part.start, part.end) == ESP_OK) {
            added += part.end - part.start;
        } else {
            LOG("ROM DRAM reclaim: heap_caps_add_region(%08x, %08x) failed\n",
                   (unsigned)part.start, (unsigned)part.end);
        }
    }
    return added;
}

#ifdef ROM_BOOT_PROBE
// Re-verification (AGENTS.md, "ROM core 1 boot DRAM"): idf.py -DROM_BOOT_PROBE=N
//   1: report what the ROM's core 1 boot writes in .data_xtos and whether
//      anything writes the reclaimed parts in the 3 s after; then allocate
//      from the reclaimed heap and check every heap;
//   2: 1, then abort() once, to check the panic reboot and core dump;
//   3: 1, then a core 1 fault 5 s after core 1 starts (core 0 should panic);
//   4: 3 without the reclaim (the control).
static uint8_t romXtosBefore[ROM_XTOS_DATA_END - ROM_XTOS_DATA_START];

// Allocate 8 KB, which the heap's good-fit search serves from the larger
// reclaimed part while that is the smallest free block that fits; report
// whether it landed there, write it, free it, and check every heap.
static void romBootProbeExercise() {
    const size_t size = 8 * 1024;
    uint8_t *block = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
    bool inPart = false;
    for (const auto &part : kRomReclaimParts) {
        inPart |= (uintptr_t)block >= part.start && (uintptr_t)block + size <= part.end;
    }
    if (block) memset(block, 0x3c, size);
    LOG("ROM_BOOT_PROBE exercise: %u-byte block at %p (%s the reclaimed parts)\n",
           (unsigned)size, block, inPart ? "in" : "NOT in");
    heap_caps_free(block);
    LOG("ROM_BOOT_PROBE heap integrity: %s\n",
           heap_caps_check_integrity_all(true) ? "ok" : "CORRUPT");
}
#endif
#endif

// Panic hook (linked with -Wl,--wrap=esp_panic_handler, main/CMakeLists.txt).
// This is a single-core FreeRTOS build, so IDF's panic path knows nothing of
// the bare-metal loop startCpu1() runs on core 1, and leaves it running:
//   - while the core dump is written to flash with the cache off, where its
//     PSRAM accesses would fault;
//   - through the panic reboot (a CPU-only reset), into the next boot, where
//     it faults on mmuState cleared by .bss init before setup() rebuilds it.
// So stop core 1 first: stall it, then hold it in reset with its clock off
// (startCpu1() then sees it stopped and restarts it normally).
//
// Also give the core dump time to finish. IDF arms the RTC watchdog for 10 s
// for the whole panic handler (port/panic_handler.c) and the core dump code
// does not feed it; erasing and writing a DRAM+PSRAM-heap dump (~2.5 MB) takes
// longer, so the reset cut the dump short (an "ffff" checksum at the next
// boot). Re-arm it for kPanicWdtMs, still a backstop against a hung handler.
#ifndef CSIM
extern "C" void esp_panic_handler_enable_rtc_wdt(uint32_t timeout_ms);
static constexpr uint32_t kPanicWdtMs = 120 * 1000;
extern "C" void __real_esp_panic_handler(void *info);
extern "C" IRAM_ATTR void __wrap_esp_panic_handler(void *info) {
    if (esp_cpu_get_core_id() == 0) {
        REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
        REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETING);
        REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
    }
#if CONFIG_ESP_COREDUMP_ENABLE
    esp_panic_handler_enable_rtc_wdt(kPanicWdtMs);
#endif
    __real_esp_panic_handler(info);
}
#endif

#ifndef CSIM
// Core 1's exceptions: every cause's C handler in the ROM's xtos table
// (xtos_c_handler_table, in the .data_xtos span setup() keeps reserved: see
// AGENTS.md, "ROM core 1 boot DRAM"), set by core 1 itself before its bus
// loop. Core 0 runs on IDF's own vectors, so this table is core 1's alone.
// Records the fault for core 0's watchdog and parks core 1 here, in IRAM.
static IRAM_ATTR void core1ExceptionHandler(void *)
{
    uint32_t epc1, cause, vaddr;
    asm volatile("rsr.epc1 %0" : "=r"(epc1));
    asm volatile("rsr.exccause %0" : "=r"(cause));
    asm volatile("rsr.excvaddr %0" : "=r"(vaddr));
    core1Fault.pc = epc1;
    core1Fault.exccause = cause;
    core1Fault.excvaddr = vaddr;
    core1Fault.valid = 1;
    for (;;) {}
}

static const char *exceptionName(uint32_t cause)
{
    switch (cause) {
    case 0: return "IllegalInstruction";
    case 2: return "InstructionFetchError";
    case 3: return "LoadStoreError";
    case 6: return "IntegerDivideByZero";
    case 9: return "LoadStoreAlignment";
    case 20: return "InstFetchProhibited";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "exception";
    }
}

// Core 1 stopped serving the bus (core 0's watchdog, core1Fault above).
static IFLASH_ATTR __attribute__((noinline, cold)) void core1Stalled()
{
    // Is the bus clock running? Core 1 isn't on the bus, so a GPIO read
    // from core 0 can't disturb it. ~8000 reads: over 100 us, many phi2
    // edges at a 6502's 1.78 MHz or the fake clock's 1.4 MHz.
    uint32_t last = REG_READ(bus.clock.regRd) & bus.clock.mask, edges = 0;
    for (int i = 0; i < 8000; ++i) {
        const uint32_t v = REG_READ(bus.clock.regRd) & bus.clock.mask;
        edges += v != last;
        last = v;
    }
    // LOG() (printf, newlib's locks) aborts with interrupts masked, as core
    // 0's loop runs: unmask them around it. Harmless here: there's no bus
    // traffic for core 0's interrupts to disturb.
    if (edges < 8) {
        if (!core1ClockStoppedLogged) {
            portENABLE_INTERRUPTS();
            LOG("core 1 watchdog: no bus cycles for %d s, and phi2 is stopped (the Atari off?): "
                "not a core 1 failure\n", core1QuietSec);
            portDISABLE_INTERRUPTS();
        }
        core1ClockStoppedLogged = true;
        core1QuietSec = 0;
        return;
    }
    // A failure exit (negative: EXIT_COREDUMP, a core dump after the report).
    if (core1Fault.valid) {
        core0_exit(1, "-10 CORE1_FAULT: core 1 %s (EXCCAUSE %u) at PC 0x%08x, EXCVADDR 0x%08x; "
                   "no bus cycles for %d s with phi2 running", exceptionName(core1Fault.exccause),
                   (unsigned)core1Fault.exccause, (unsigned)core1Fault.pc, (unsigned)core1Fault.excvaddr,
                   core1QuietSec);
        return;
    }
    // A hang: where core 1 is, from the assist-debug unit's PC record
    // (turned on only now, so it can't touch core 1's timing).
    REG_SET_BIT(SYSTEM_CPU_PERI_CLK_EN_REG, SYSTEM_CLK_EN_ASSIST_DEBUG);
    REG_CLR_BIT(SYSTEM_CPU_PERI_RST_EN_REG, SYSTEM_RST_EN_ASSIST_DEBUG);
    REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_PDEBUGENABLE_REG, 1);
    REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_RECORDING_REG, 1);
    uint32_t pcs[4];
    for (auto &pc : pcs) {
        const uint32_t t0 = XTHAL_GET_CCOUNT();
        while (XTHAL_GET_CCOUNT() - t0 < 240 * 1000) {}   // 1 ms
        pc = REG_READ(ASSIST_DEBUG_CORE_1_RCD_PDEBUGPC_REG);
    }
    core0_exit(1, "-10 CORE1_HANG: no bus cycles for %d s with phi2 running, no fault recorded; "
               "core 1 PC 0x%08x 0x%08x 0x%08x 0x%08x (stalled: %s)", core1QuietSec, (unsigned)pcs[0],
               (unsigned)pcs[1], (unsigned)pcs[2], (unsigned)pcs[3],
               REG_GET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL) ? "yes" : "no");
}
#endif

void IFLASH_ATTR startCpu1() {  
    if (REG_GET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN)) {
        LOG("APP CPU is already running!\n");
        return;
    }

    DPORT_REG_WRITE(SYSTEM_CORE_1_CONTROL_1_REG, 0);
    DPORT_REG_WRITE(SYSTEM_CORE_1_CONTROL_0_REG, 0);
    DPORT_REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETING);
    DPORT_REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETING);

    ets_set_appcpu_boot_addr((uint64_t)&app_cpu_init);
    DPORT_REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
}


#include <fcntl.h>
#include "esp_err.h"
#include "esp_log.h"


// SystemControl, the Sidecar's bus sampler: a period of category, or stop.
// Main starts category 0 when core 1 takes the bus and stops it before its
// exit report (the last period is printed then). Hot-safe.
IRAM_ATTR void sidecarSampling(SidecarSampleAction action, uint8_t category)
{
#if A8BB_SIDECAR
    Sidecar &sidecar = sidecarHost.sidecar();
    if (sidecar.present() && !sidecar.loopback())
        sidecar.systemControl(0, 0, action, category, 200000);
#else
    (void)action;
    (void)category;
#endif
}

// SystemControl, the Sidecar's system flags (SidecarSystemFlag): set, then
// clear, these bits; the others unchanged. Hot-safe.
IRAM_ATTR void sidecarSystemFlags(uint32_t set, uint32_t clear)
{
#if A8BB_SIDECAR
    Sidecar &sidecar = sidecarHost.sidecar();
    if (sidecar.present() && !sidecar.loopback())
        sidecar.systemControl(set, clear, SidecarSampleAction::None, 0, 200000);
#else
    (void)set;
    (void)clear;
#endif
}

// The boot profile's cartridge (also a cartridge change): a local VFS path,
// or "sidecar!<path>", fetched from the Sidecar's storage over the link
// (its path resolved as for "sidecar!" diskSpecs, e.g.
// "sidecar!/sd/SDX451_atarimax128.car.gz"). Cold: setup(), or with the bus
// detached.
void openCartImage(const std::string &spec)
{
    static constexpr std::string_view kSidecarPrefix = "sidecar!";
    if (spec.starts_with(kSidecarPrefix)) {
        const std::string path = spec.substr(kSidecarPrefix.size());
#if A8BB_SIDECAR
        uint8_t *data = nullptr;
        size_t size = 0;
        if (sidecarHost.fetchFile(path, 1024 * 1024, &data, &size)) {
            atariCart.openImage(path.c_str(), data, size);
            heap_caps_free(data);
            return;
        }
        LOG("cartImage '%s': not fetched from the Sidecar; no cartridge\n", spec.c_str());
#else
        LOG("cartImage '%s': no Sidecar in this build; no cartridge\n", spec.c_str());
#endif
        atariCart.close();
        return;
    }
    atariCart.open(sio.resolve(spec).c_str());
}

#if !A8BB_SIDECAR && !defined(GPIO19_IN_USE) && !defined(CSIM)
// Wait, at most 2 s, until a host is reading the USB console.
// IDF's USB-Serial-JTAG console drops all output (write() fails) while it
// thinks no host is attached, and it decides that in a FreeRTOS tick hook
// (no USB SOF for 4 ms). After boot.sh's flash, the chip's reset re-
// enumerates the USB, so the flag goes false at boot. If the host isn't
// back before BOOT COMPLETE, where core 0 masks interrupts for the run, no
// tick ever sets it true again: the whole run is silent (BOOT COMPLETE
// included, so boot.sh never powers on the Atari) until the exit report
// re-enables interrupts. A reader matters too: lines written before
// boot.sh's cat opens the port are dropped. So: wait for SOFs, then for the
// host to take a packet (fsync() returns 0).
// Also at the exit report: interrupts masked for the whole run leave the
// console dropping output once they're back, until a tick sees the host.
static void waitForUsbConsoleHost(const char *when, uint32_t timeoutMs)
{
    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (usb_serial_jtag_is_connected()) {
            printf("\n");
            fflush(stdout);
            clearerr(stdout);
            if (fsync(fileno(stdout)) == 0) {
                LOG("USB console (%s): host reading after %u ms\n", when, (unsigned)(millis() - start));
                return;
            }
        }
        delay(10);
    }
    clearerr(stdout);
    LOG("USB console (%s): no host reading after %u ms; going on\n", when, (unsigned)timeoutMs);
}
#endif

void setup() {
    if (setupComplete)
        return;
#if defined(IRQ_JAM_TEST) && IRQ_JAM_TEST > 0
    // -DIRQ_JAM_TEST=<sec>: the IRQ jam from here, lifted <sec> seconds into
    // core0Loop() (a test of the jam itself).
    irqJam();
#endif
#if !A8BB_SIDECAR && !defined(GPIO19_IN_USE) && !defined(CSIM)
    waitForUsbConsoleHost("boot", 2000);   // then go on: boot.sh powers on blind after 10 s
#endif
#ifdef ATARI800_SIM
    setupAtariRam = MEMORY_mem;
    setupAtariRamSize = 65536;
    atariRam = MEMORY_mem;
#endif
    for(auto i : gpios)
        if (!(irqJammed && i == bus.irq_.pin)) pinMode(i, INPUT);
#if defined(GPIO19_IN_USE) && !defined(CSIM)
    // refresh_ on GPIO19, main's USB D-: the USB-Serial-JTAG PHY owns the
    // pad (and the ROM turned it on), so turn the pad and its pull-ups off
    // and make it a plain input. From here main has no USB console; LOG()
    // goes through the Sidecar.
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE |
        USB_SERIAL_JTAG_DP_PULLUP | USB_SERIAL_JTAG_DM_PULLUP | USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
    pinMode(bus.refresh_.pin, INPUT_PULLUP);   // (the bench: nothing drives it)
#endif
#ifdef PROFILE_GPIO_SAMPLE
    profileGpioInitCore0();
#endif
    pinMode(bus.halt_.pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(bus.halt_.pin, 0);
#if defined(SLED_TEST) && SLED_TEST
    // No startup halt: the pin set up (open drain, low) but not driven;
    // core 1's page bits still drive it for halting writes. On a 130XE the
    // ECI pin it's wired to is ANTIC's HALT output, and pulling it low
    // under a running machine (a reboot of main mid-run) crashed it (the
    // pool, 2026-11-26); the sled catches the 6502 without the halt.
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.halt_.mask);
#elif defined(CSIM)
    // The startup halt, as the pin has it: held until core 1 releases it
    // (halt_ into pinReleaseMask); csim's bus cycles track only what they
    // write, so core 0's write goes to them too (as irqJam()'s).
    csim_core0_gpio_enable1(bus.halt_.mask, 0);
#endif
#ifndef BOOT_CONFIG
#define BOOT_CONFIG ""
#endif
    config.load(BOOT_CONFIG);
    if (config.startupDelayMs > 0) delay(config.startupDelayMs);


#if HAS_STATUS_LED
    led.init();
    led.write(20, 0, 0);
#endif
    //delay(500);
    //printf("setup()\n");
#if 0
    ledcAttachChannel(43, testFreq, 1, 0);
    ledcWrite(0, 1);

    while(1) { 
        pinMode(44, OUTPUT);
        pinMode(0, INPUT);
        digitalWrite(44, 1);
        ledcWrite(0, 0);
        delay(500);
        digitalWrite(44, 0);
        ledcWrite(0, 1);
        delay(500);
        LOG("OK %d %d\n", digitalRead(44), digitalRead(0));
    }
#endif
    if (0) { 
        for(auto p : gpios) pinMode(p, INPUT);

        while(1) { 
            for(auto p : gpios) {
                LOG("%02d:%d ", p, digitalRead(p));
            }
            LOG("\n");
            delay(50);
        }
    }

    //for(auto i : gpios) pinMode(i, INPUT);

#ifndef GPIO19_IN_USE   // (pinDefs.h: the install turns the USB pads back on)
    usb_serial_jtag_driver_config_t jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&jtag_config);
#endif

    if (opt.testPins) { 
        for(auto p : gpios) pinMode(p, INPUT_PULLUP);
        while(1) { 
            for(auto p : gpios) {
                LOG("%02d:%d ", p, digitalRead(p));
            }
            LOG("\n");
            delay(200);
        }
    }
    

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = false
    };
    LOG("mounting spiffs...\n");
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_FAIL) {
        // Not a SPIFFS (a new board's erased or never-written partition):
        // format it, so main always comes up, and the Sidecar (its link
        // begins just below) can fill it in diagnostic mode, or with the
        // console's "flash spiffs". Several seconds, first boot only.
        LOG("spiffs: no filesystem; formatting it\n");
        conf.format_if_mount_failed = true;
        const uint32_t t0 = millis();
        ret = esp_vfs_spiffs_register(&conf);
        LOG("spiffs: format %s in %u ms\n", ret == ESP_OK ? "done" : esp_err_to_name(ret),
            (unsigned)(millis() - t0));
    }
    while (ret != ESP_OK) {
        LOG("Could not mount or format spiffs! (%s)\n", esp_err_to_name(ret));
        delay(500);
    } 
    LOG("mounted\n");
#if A8BB_SIDECAR
    // The Sidecar link, early: the boot report, diagnostic mode (main's
    // files need the spiffs above, nothing below), and the Sidecar's
    // storage for what setup() loads next (a "sidecar!" cartridge).
    sidecarHost.begin();   // probes, sends the boot report, takes the Sidecar's reply
#ifndef CSIM
    if (sidecarHost.diagnostic())
        sidecarDiagnosticMode();   // never returns: the bus is never attached
#endif
#endif

    // This boot's configuration (its Atari half), now the link is up and
    // spiffs mounted: everything from here (memory, cartridge, drives) is
    // set by it.
    chooseBootConfig();
#if defined(SLED_TEST) && SLED_TEST
    // The sled catches a running 6502, so the Atari is powered on before it,
    // at the first line main's output reaches the Sidecar's console (the
    // link is up): boot.sh powers it on at BOOT COMPLETE
    // (tools/power_on_when_up.sh), which otherwise comes after the sled has
    // waited for a 6502 still off and timed out.
    LOG("boot config: %s\n", bootConfigSummary.c_str());
    LOG("BOOT COMPLETE\n");
    sidecarSystemFlags(static_cast<uint32_t>(SidecarSystemFlag::BootComplete), 0);
#endif
    if (config.haltAvailable == false) {
        mmuState.pinReleaseMask |= bus.halt_.mask;
    } else { 
        mmuState.pinReleaseMask &= bus.halt_.maskInverse;
    }
    baseMemSz = config.baseMemSz; 
    ESP_ERROR_CHECK(baseMemSz > 0 && baseMemSz % 256 == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE);
    if (setupAtariRam != nullptr) {
        // csim: atari800's 64K backs atariRam; the board serves its first
        // baseMemSz bytes, atari800's native RAM (its -ram) answers above.
        if (setupAtariRamSize < static_cast<size_t>(baseMemSz))
            abort();
        atariRam = setupAtariRam;
    } else {
        // AUTO memory: atariRam is only the first 2K, which stays; mmuInit()
        // maps the rest in pieces that autoMemConfig() can free.
        const int atariRamSz = config.autoMem ? 2 * 1024 : baseMemSz;
        atariRam = (uint8_t *)heap_caps_aligned_alloc(16,
                                                      atariRamSz,
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        assert(atariRam != NULL);
        bzero(atariRam, atariRamSz);
        // TODO: still paranoid about OS peeking under basic ROM and thinking there
        // is a cartridge there because of 0-intialized RAM
        if (baseMemSz > 0xbffc) atariRam[0xbffc] = 0xff;

    }
    atariMem.dcb = (AtariDCB *)&atariRam[0x300];
    atariMem.ziocb = (AtariIOCB *)&atariRam[0x20];
    atariMem.iocb0 = (AtariIOCB *)&atariRam[0x320];
    if (!extMem.init(16, config.extMemSramBanks, config.extMemConf))
        LOG("Extended memory unavailable; continuing without it\n");

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        LOG("Failed to get SPIFFS partition information (%s)\n", esp_err_to_name(ret));
    } else {
        LOG("Partition size: total: %zu, used: %zu\n", total, used);
    }

    psram = (uint32_t *) heap_caps_aligned_alloc(64, psram_sz,  MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    psram_end = psram + (psram_sz / sizeof(psram[0]));
    if (psram != NULL)
        bzero(psram, psram_sz);


    LOG("cartImage='%s'\n", config.cartImage.c_str());
#if CONFIG_A8BB_SYSMONITOR
    sysMonitor = new SysMonitor();
#endif
//    initProcFsNodes();
    cioFiles = new CioDispatcher();
    // Off while soak testing the struct logs' removal (809b496) on test
    // devices 5 and 6; structLogsInit() turns them on.
    structLogsInit();
    sio.setNetworkUp(wifiRun);
    sio.setProcFsNodes(&procFsNodes);
    // The drives are mounted below, after sidecarHost.begin().
    openCartImage(config.cartImage);
    pbiROM[DISABLE_BASIC - PBIROM_BASE] = (atariCart.bankA0 >= 0 || config.enableBasic == false);

#if 0 //ndef BOOT_SDX
    // 169572 before sdkconf changes
    // 174595 after malloc and malloc 0 changes
    // 91719 with wiFi 
    // 92207 after lwip and wifi changes

    connectWifi(); // 82876 bytes 
#endif

    while(0) { 
        yield();
        delay(500);
        LOG("OK\n");
    }
    //for(auto i : gpios) pinMode(i, INPUT);
    while(opt.watchPins) { 
            delay(100);
            LOG("PU   %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG));
    }

#ifdef FAKE_CLOCK
    setupFakeBusInjection();
#endif

    //pinDisable(bus.extDecode.pin);
    for(int i = 0; i < 1; i++) { 
        LOG("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    }
    LOG("freq %.4fMhz threshold %d halfcycle %d psram %p\n", 
        testFreq / 1000000.0, lateThresholdTicks, (int)halfCycleTicks, psram);

    esp_rom_gpio_connect_in_signal(bus.clock.pin, CORE1_GPIO_IN0_IDX, false);
#ifdef PROFILE_GPIO_SAMPLE
    profileGpioInitCore1();
#endif
    if (!irqJammed) {   // jammed: irqJam() set the pin up, held low
    digitalWrite(bus.irq_.pin, 1);
    pinMode(bus.irq_.pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(bus.irq_.pin, 1);
    }
    //initLed();
#if HAS_STATUS_LED
    led.write(0, 20, 0);
#endif

    if (!irqJammed) {
    pinMode(bus.irq_.pin, OUTPUT_OPEN_DRAIN);
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.irq_.mask);
    digitalWrite(bus.irq_.pin, 0);
    }

#ifdef FAKE_CLOCK
    pinMode(bus.extSel.pin, OUTPUT_OPEN_DRAIN | PULLUP);
#else
    pinMode(bus.extSel.pin, OUTPUT_OPEN_DRAIN);
#endif
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.extSel.mask);
    digitalWrite(bus.extSel.pin, 0);
    // extSel_ and mpd_ from core 1's dedicated output channels (pinDefs.h),
    // inverted so 1 pulls low. Core 1's channel outputs reset to 0, i.e.
    // released, so the route can go in before the output enable. The pads
    // stay open drain; OEN_SEL makes each enable its GPIO_ENABLE1 bit, which
    // stays on: pinReleaseMask doesn't include them. mpd_ had no explicit
    // pin setup before (GPIO_OUT1 low, its enable bit toggled).
#ifdef FAKE_CLOCK
    pinMode(bus.mpd.pin, OUTPUT_OPEN_DRAIN | PULLUP);
#else
    pinMode(bus.mpd.pin, OUTPUT_OPEN_DRAIN);
#endif
#ifndef CSIM   // csim has no pads or GPIO matrix
    if (!(REG_READ(SYSTEM_CPU_PERI_CLK_EN_REG) & SYSTEM_CLK_EN_DEDICATED_GPIO))
        LOG("dedicated GPIO clock was off\n");
    REG_SET_BIT(SYSTEM_CPU_PERI_CLK_EN_REG, SYSTEM_CLK_EN_DEDICATED_GPIO);
    REG_CLR_BIT(SYSTEM_CPU_PERI_RST_EN_REG, SYSTEM_RST_EN_DEDICATED_GPIO);
    esp_rom_gpio_connect_out_signal(bus.extSel.pin, CORE1_GPIO_OUT0_IDX + bus.ctrlExtSel.shift, true, false);
    REG_SET_BIT(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * bus.extSel.pin, GPIO_FUNC0_OEN_SEL);
    esp_rom_gpio_connect_out_signal(bus.mpd.pin, CORE1_GPIO_OUT0_IDX + bus.ctrlMpd.shift, true, false);
    REG_SET_BIT(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * bus.mpd.pin, GPIO_FUNC0_OEN_SEL);
#endif
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, bus.extSel.mask | bus.mpd.mask);
    LOG("extSel_ and mpd_ on core 1 dedicated channels %d and %d\n",
           bus.ctrlExtSel.shift, bus.ctrlMpd.shift);

    pinMode(bus.halt_.pin, OUTPUT_OPEN_DRAIN);
#if defined(SLED_TEST) && SLED_TEST
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.halt_.mask);   // no startup halt (above)
#endif
    //REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.halt_.mask);
    //digitalWrite(bus.halt_.pin, 0);

    for(int i = 0; i < 8; i++) {
#ifdef FAKE_CLOCK
        pinMode(bus.data.pin + i, OUTPUT | PULLUP); // TODO: Investigate OUTPUT_OPEN_DRAIN doesn't work, would enable larger page sizes if it did
#else
        pinMode(bus.data.pin + i, OUTPUT);
#endif
    }
    clearInterrupt();
    mmuInit();
#ifdef FAKE_CLOCK
    setupFakeBusMmu();
#endif
#ifdef CSIM
    // Why isn't this sufficient
    mmuState.l1Blocks[l1BlockC0] = mmuState.osEnL1BlockMux[0]; // OS disabled
#endif
    telnetScreenBegin();   // the Sidecar's telnet client, over the link (no-op without it)
    screenshotStreamBegin();   // screenshots for the Sidecar (SidecarMainFlag::Screenshot)
    mountDiskSpecs(config.diskSpec);
#if (defined(TEST_CORE1_FAULT) || defined(TEST_CORE1_HANG)) && !defined(CSIM)
    testCore1Armed = esp_reset_reason() != ESP_RST_PANIC;
    LOG("TEST_CORE1_%s: core 1 %s after %d s%s\n",
#ifdef TEST_CORE1_FAULT
        "FAULT", "faults", TEST_CORE1_FAULT,
#else
        "HANG", "stops", TEST_CORE1_HANG,
#endif
        testCore1Armed ? "" : " (not this boot: it followed a panic)");
#endif
#if defined(TEST_PANIC) && TEST_PANIC > 0 && !defined(CSIM)
    testPanicArmed = esp_reset_reason() != ESP_RST_PANIC;
    LOG("TEST_PANIC: core 0 crashes after %d s%s\n", TEST_PANIC,
        testPanicArmed ? "" : " (not this boot: it followed a panic)");
#endif
    //if (sio.disk(2) == NULL)
    //    sio.mount(2, "smb://miner6.local/pub");
    //if (sio.disk(3) == NULL)
    //    sio.mount(3, "proc:");
    mmuDebugPrint();
    enableBus();
#if defined(SLED_TEST) && SLED_TEST
    sledPrepare();
#endif
    // WiFi's driver memory now, before the heap report, the ROM DRAM
    // reclaim guard and core 1, and before AUTO memory
    // sizes its banks at PBI init; the radio start and connect stay deferred.
#if CONFIG_A8BB_WIFI
    if (config.enableWifi)
        wifiPreinit();
#endif
    logHeapInfo(MALLOC_CAP_SPIRAM);
    logHeapInfo(MALLOC_CAP_INTERNAL);
    reportHotDataBanks();
#if !defined(CSIM) && CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
    romReclaimFill(kRomReclaimPattern);
#ifdef ROM_BOOT_PROBE
    memcpy(romXtosBefore, (const void *)ROM_XTOS_DATA_START, sizeof(romXtosBefore));
#endif
#endif
#ifdef FAKE_SIO
    fakeSioInit();   // allocates and prints: before core 1 and the masked loop
#endif
    startCpu1();
    busywait(.01);
    sidecarSampling(SidecarSampleAction::Start, 0);   // the bus is ours from here
#if !defined(CSIM) && CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
#ifdef ROM_BOOT_PROBE
    while (!appCpuRunning) {}
    LOG("ROM_BOOT_PROBE: %u bytes of .data_xtos changed by core 1 boot\n",
           (unsigned)romSpanChanges(".data_xtos", ROM_XTOS_DATA_START, ROM_XTOS_DATA_END, 0,
                                    romXtosBefore));
    LOG("ROM_BOOT_PROBE: %u bytes of the reclaimed parts changed by core 1 boot\n",
           (unsigned)romReclaimChanges("boot", kRomReclaimPattern));
    busywait(3);
    LOG("ROM_BOOT_PROBE: %u bytes of the reclaimed parts changed 3 s later\n",
           (unsigned)romReclaimChanges("3 s", kRomReclaimPattern));
#endif
    {
        const size_t gap = mmuHotDataGapToHeap();
        LOG("Core 1 hot data at a bank start; %u bytes before it to the heap\n", (unsigned)gap);
    }
    {
        const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#if defined(ROM_BOOT_PROBE) && ROM_BOOT_PROBE == 4
        const size_t reclaimed = 0;   // the control: keep the span reserved
#else
        const size_t reclaimed = reclaimRomAppCpuBootDram();
#endif
        LOG("Reclaimed %u bytes of ROM core 1 boot DRAM; internal heap free %u -> %u\n",
               (unsigned)reclaimed, (unsigned)internalBefore,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
#ifdef ROM_BOOT_PROBE
    romBootProbeExercise();
#if ROM_BOOT_PROBE >= 3
    // Core 1 faults 5 s after it starts: core 0 should panic about then.
    for (int i = 0; i < 4; ++i) {
        busywait(2);
        LOG("ROM_BOOT_PROBE: %d s, no panic yet; heap integrity %s\n", 2 * (i + 1),
               heap_caps_check_integrity_all(true) ? "ok" : "CORRUPT");
    }
#endif
#if ROM_BOOT_PROBE == 2
    {
        static RTC_NOINIT_ATTR uint32_t probeAborts;
        if (esp_reset_reason() != ESP_RST_PANIC) probeAborts = 0;
        if (probeAborts++ == 0) {
            LOG("ROM_BOOT_PROBE: abort() to exercise the panic path\n");
            abort();
        }
        LOG("ROM_BOOT_PROBE: booted after the panic; not aborting again\n");
    }
#endif
#endif
#endif
#ifndef CSIM
    //xTaskCreatePinnedToCore(threadFunc, "core0Loop", 12 * 1024, NULL, 0, NULL, 0);
    threadFunc(nullptr);
    while(1) { yield(); delay(1000); };
#endif
    setupComplete = true;
}
        
void loop() {
    while(1) { yield(); delay(1); }
}

static void app_cpu_main() {
#ifndef CSIM
    uint32_t oldint;
    //XT_INTEXC_HOOK oldnmi = _xt_intexc_hooks[XCHAL_NMILEVEL];
    //_xt_intexc_hooks[XCHAL_NMILEVEL] = my_nmi;  // saves 5 cycles, could save more 
    //
    ASM("rsil %0, 15" : "=r"(oldint) : : );
    // Core 1's exceptions to core1ExceptionHandler() (core 0's watchdog
    // reports them), not the ROM's fatal handler, which spins silently.
    for (int cause = 0; cause < 64; ++cause)
        _xtos_set_exception_handler(cause, reinterpret_cast<_xtos_handler>(core1ExceptionHandler));
    // Out of the ROM and masked: core 1 no longer uses the ROM's DRAM (not
    // even its interrupt handler table), so setup() may reclaim it.
    appCpuRunning = true;
#if defined(ROM_BOOT_PROBE) && ROM_BOOT_PROBE >= 3
    {   // ROM_BOOT_PROBE 3/4: a core 1 fault 5 s in, through the ROM's vectors
        const uint32_t t0 = XTHAL_GET_CCOUNT();
        while (XTHAL_GET_CCOUNT() - t0 < 5u * 240u * 1000u * 1000u) {}
        volatile uintptr_t bad = 4;
        *(volatile uint32_t *)bad = 0;
    }
#endif
    if (atariCart.header.type == AtariCart::MegaCart128)
        iloop_pbi<CartBankSelect::D500_DATA_BANK_SEL>();
    else
        iloop_pbi<CartBankSelect::D500_ADDR_BANK_SEL>();
    while(1) {}
#endif
}


// https://www.oocities.org/dr_seppel/pbi1_eng.htm
// https://www.oocities.org/dr_seppel/pbi2_eng.htm
// https://github.com/maarten-pennings/6502/blob/master/4ram/README.md

//PBI pins
// A0-15
// D0-7
// _ExtSel - override RAM access
// _MPD
// _CasInh - output 0 = atari is reading ram, 1 atari is reading a rom 


// D1FF NEWPORT
// D301 PORTB and MMU  bit 0 - OS ROMS enable, 1 basic ROM enable, 2 1200XL leds, 4-5 130xe bank switch, 5000-57FF RAM 
// D800-DFFF    Math ROM  
// D800 ROM cksum lo
// D801 ROM cksum hi
// D802 ROM version
// D803 ID num
// D804 Device Type
// D805 JMP (0x4C)
// D809 ISR vect LO
// D80A ISR vect HI
// D80B ID num 2 (0x91)
// D80C Device Name (ASCII)
// D80A-D818 device vectors
// D819 JMP ($4C)
// D820 Init Vector LO
// D821 Init vector hi

// OS sequentally sets each bit in NEWPORT, then 
//OS checks D808 for 0x4C and D80B for 0x91, then jumps to D819 
// Actual bus trace shows it seems to read D803 checking for == 80
//RAM MAP
// $8000-9FFF
// $A000-BFFF

// $C000-FFFF OS ROM
// $D000-D7FF 2K for mmapped chips, GTIA, POKEY, PIA, ANTIC
// $D000 GTIA  
// $D200 POKEY 
// $D300 PIA
// $D400 ANTIC

// need PBI lines extDecode, WRT, phi2, ADDR0-15, DATA0-7, EXTSEL

// NOTES:
// 8-pin i2c io expander: https://media.digikey.com/pdf/Data%20Sheets/NXP%20PDFs/PCF8574(A).pdf
// TODO: verify polarity of RW, MPD, casInh, etc 


int LineBuffer::add(char c, std::function<void(const char *)> f/* = NULL*/) {
        int r = 0;
        if (c != '\r' && c != '\n')
                line[len++] = c; 
        if (len >= sizeof(line) - 1 || c == '\n') {
                r = len;
                line[len] = 0;
                len = 0;
                if (f != NULL) { 
                        f(line);
                }
        }
        return r;
}

#ifdef ATARI800_SIM
// csim's atari800 interface, the app's side (main.h; the board's side is
// csim/a800/csim_atari800.cpp).

#ifndef CSIM_BUS_CYCLE
// The default mode's core 0: one pass of csim's own pieces of it, in line
// on each CPU tick. (Bus-cycle mode runs the real core0Loop() instead.)
void csim_app_core0_pass() {
    const uint64_t now = CSIM_PEEK_CCOUNT64();
    elapsedSec = static_cast<int>(now / (240ULL * 1000000ULL));

    static uint64_t lastInterruptTsc = 0;
    if (config.interruptTicks > 0 && !interruptRequested && ioCount > 0
        && now - lastInterruptTsc > static_cast<uint64_t>(config.interruptTicks)) {
        lastInterruptTsc = now;
        interruptRequested = 1;
        atariRam[PDIMSK] |= pbiDeviceNumMask;
    }
#ifdef FAKE_SIO
    // As core0Loop() does: the fake 6502 every FAKE_SIO_TICK_US.
    static uint64_t lastFakeSioTsc = 0;
    if (now - lastFakeSioTsc > 240ULL * FAKE_SIO_TICK_US) {
        core0_fakeSioTick();
        lastFakeSioTsc = now;
    }
#endif
    static uint64_t lastUtilityTsc = 0;
    if (now - lastUtilityTsc > bmonTimeout) {
        core0_utilityTasks();
        lastUtilityTsc = now;
    }
    if (exitFlag) { 
#ifdef CSIM
        sio.reportStats();
#endif
#ifdef FAKE_SIO
        fakeSioReport();
#endif
        a8logReport();
        LOG("DONE %-10.2f %s\nExit reason %d %s\n", millis() / 1000.0, exitReason.c_str(), exitFlag, exitReason.c_str());
        a8logFlush();
        exit(0);
    }
}
#else
// threadFunc() has returned on core 0's coroutine: the run is over.
void csim_app_finish() {
    sio.reportStats();
    LOG("Exit reason %d %s\n", exitFlag, exitReason.c_str());
    fflush(stdout);
    exit(0);
}
#endif

int csim_app_irq() {
    return interruptRequested != 0 || irqJammed;   // the request, or the jam (irqJam())
}

bool csim_app_setup_done() {
    return setupComplete;
}

// csim's coarse main reboot (csim/a800/csim_atari_reset.cpp; core 0's
// coroutine already abandoned): LOG() off the link, main's link end closed
// (begin() opens it again), and setup() again. Globals and the heap carry
// over: not a fresh boot, a re-run of the boot sequence.
#ifdef CSIM_BUS_CYCLE
void csim_app_reboot() {
#if A8BB_LOG_VIA_SIDECAR
    a8logSidecarStop("csim main reboot");
#endif
    sidecarHost.sidecar().end();
    mmuStaticReinit();
    setupComplete = false;
    rebootRequested = 0;
    // The run's clock from 0, as a real boot's: the boot keys (typed at
    // elapsedSec 15) and the per-second tests go by it.
    elapsedSec = 0;
    LOG("csim: main reboot: setup() again\n");
    setup();
}
#endif

void csim_app_setup(uint8_t *ram, size_t ramSize) {
    if (setupComplete)
        return;
    setupAtariRam = ram;
    setupAtariRamSize = ramSize;
    atariRam = ram;
    setup();
}
#endif
