#ifdef CSIM
#include "csim.h"
#endif

#pragma GCC optimize("Ofast")
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
#include <time.h>   // libsmb2's headers use time_t without including it
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#include "hostSio.h"
#include "diskFlash.h"
#include "diskStitch.h"
#include "procFsNodes.h"
#include "cartridge.h"
#include "sysMonitor.h"
#include "sysConfig.h"
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
#include "hotData.h"


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
            sio.mount(d, ds);
            continue;
        }
        sio.unmount(d);   // the drive is the Sidecar's; nothing local
        const std::string sidecarSpec = ds.substr(kSidecarPrefix.size());
#if CONFIG_A8BB_SIDECAR_ENABLE
        sidecarHost.mount(d + 1, sidecarSpec);
#else
        printf("D%d: '%s': no Sidecar in this build (A8BB_SIDECAR_ENABLE)\n", d + 1,
               sidecarSpec.c_str());
#endif
    }
}

DRAM_ATTR LedRmt led;  

DRAM_ATTR int deferredInterrupt = 0, interruptRequested = 0, sysMonitorRequested = 0;

IRAM_ATTR void raiseInterrupt() {
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

IRAM_ATTR void clearInterrupt() { 
    for(auto &b : l1Blocks)
        for (auto &ctrl : b.ctrl) 
            ctrl &= bus.irq_.maskInverse;
    busyWait6502Ticks(2);
    mmuState.pinReleaseMask |= bus.irq_.mask;
    interruptRequested = 0;
    busyWait6502Ticks(10);
    d000Read[_0x1ff] = 0x0;
    atariRam[PDIMSK] &= pbiDeviceNumMaskNOT;
}

IRAM_ATTR void resume6502() { 
    bmonTail = bmonHead;    
    mmuState.pinReleaseMask |= bus.halt_.mask;
    // Blind: bmon records only writes, so it no longer counts bus cycles.
    busyWait6502Ticks(15);
    mmuState.pinReleaseMask &= haltMaskNOT;
}

static DRAM_ATTR uint8_t savedD5Offset = 0;
static constexpr DRAM_ATTR int page01 = pageNr(0x100);
static constexpr DRAM_ATTR int l1Block01 = page2L1Block(page01);

IRAM_ATTR void enableBus() {
    //busWriteDisable = 0;
    //pinEnableMask = _0xffffffff;
    busyWait6502Ticks(20);
    lastPageOffset[pageD5] = savedD5Offset;
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
    savedD5Offset = lastPageOffset[pageD5];
    //busWriteDisable = 1;
    //pinEnableMask = bus.halt_.mask;
    //busyWait6502Ticks(20);
}

DRAM_ATTR static const int psram_sz =  32 * 1024;
DRAM_ATTR uint32_t *psram;
DRAM_ATTR uint32_t *psram_end;

DRAM_ATTR static const int testFreq = fakeClockHz;   // fakeBus.h
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
    printf("connect() returned %d\n", r);
    uint32_t startMs = millis();
    int count;
    int sent = 0;
    while(sent < len) { 
        if (!wc.connected()) { 
            printf("lost connection");
            return false;
        }
        int pktLen = min(txSize, len - sent);
        r = wc.write((uint8_t *)(buf + sent), pktLen);
        if (r != pktLen) {
            printf("write %d returned %d\n", count, r);
            return false;
        }
        sent += r;
        if (count++ % 100 == 0) { 
            printf("."); 
            fflush(stdout);
        }
        if (resetWdt) wdtReset();
        yield();
    }
    printf("\nDone %.3f mB/sec\n", psram_sz * sizeof(psram[0]) / 1024.0 / 1024.0 / (millis() - startMs) * 1000.0);
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

IRAM_ATTR void putKeys(const char *s, int len) { 
    simulatedKeyInput.putKeys(s, len);
}
IRAM_ATTR void putKey(char c) { 
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
        printf("structLogs: no %u bytes of internal RAM, struct logs off\n",
               (unsigned)sizeof(StructLogs));
    }
}

void setup();

static uint8_t *setupAtariRam = nullptr;
static size_t setupAtariRamSize = 0;
static bool setupComplete = false;
const static DRAM_ATTR uint32_t bmonTimeout = 240 * 1000 * 10;

extern DRAM_ATTR int httpRequests;

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

void CORE0_HOTSPOTS_ATTR core0_onBmonChange() { 
    unsigned int bHead = bmonHead;
    if (bHead == bmonTail)
        return;

    PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];

    //bmonMax = max((bHead - bmonTail) & bmonArraySzMask, bmonMax);
    uint32_t bmon = bmonArray[bmonTail].r0 & bmonMask;
    // Consume the entry now, not at the end: handlers below that discard the
    // backlog (bmonTail = bmonHead, here or in resume6502()) must have the
    // last word. A closing bmonTail++ after them left the tail one past the
    // head, and core 0 then replayed the whole stale ring (~511 old entries,
    // racing core 1's overwrites), re-running mmuOnChange() on stale writes.
    bmonTail = (bmonTail + 1) & bmonArraySzMask;

    uint32_t r0 = bmon >> bmonR0Shift;

    uint16_t addr = (r0 & bus.addr.mask) >> bus.addr.shift;
    if ((r0 & bus.rw.mask) == 0) {
        uint32_t lastWrite = addr;
        if ((lastWrite & _0xff00) == _0xd500 && atariCart.accessD500(lastWrite)) {
        } else if (lastWrite == _0xd301) { 
            // TODO - trying this to see if core1 is late with this remap
            // ANSWER - it does not 
            mmuState.l1Blocks[l1Block40] = mmuState.extBanks[d000Write[_0x301]];

            mmuOnChange();
            bmonTail = bmonHead;
        } else if (lastWrite == _0xd1ff) {
            mmuOnChange();
            bmonTail = bmonHead;
        } else if (lastWrite == _0xd830 && pbiRequest[0].req != 0) {
            handlePbiRequest(&pbiRequest[0]);
            bmonTail = bmonHead;
        } else if (lastWrite == _0xd840 && pbiRequest[1].req != 0) {
            handlePbiRequest(&pbiRequest[1]);
            bmonTail = bmonHead;
        }
        // these pages have pins.halt.mask set in the page enable table and will halt the 6502 on any write.
        // restart the 6502 now that onMmuChange has had a chance to run. 
        if (//pageNr(lastWrite) == pageNr_d500 ||
            pageNr(lastWrite) == pageNr_d301 ||
            pageNr(lastWrite) == pageNr_d1ff ||
            false
        ) {
            // Don't know why it hangs without this wait (it was bmonWaitCycles(2),
            // 2 bus cycles counted in bmon).  This is still very much the case on
            // both 130XE and 600XL
            busyWait6502Ticks(2); 

            if (config.haltAvailable) { 
                haltCount++; 
                resume6502();
            }
        }
        // repeatedBrokenRead detection is disabled below.
    }
#ifdef HAVE_REFRESH
    else if ((r0 & bus.refresh_.mask) != 0) {
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
#endif  // HAVE_REFRESH
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
}
// Core 0's housekeeping between bus monitor passes: simulated key input, PBI
// interrupts, the once-a-second checks (elapsedSec, the watchdog, exit
// conditions). core0Loop() calls it once per bmonTimeout pass (about every
// 50 ms) on hardware, and csim_app_cpu_tick() calls it at the same interval.
// Always inlined, so core0Loop() keeps the code it had when this was
// written out in its loop. An exit condition calls core0_exit() (exitReason
// and exitFlag) and returns true at once, so the first one to fire names
// the exit and core0Loop() breaks out of its loop, as the inline code did.
// csim, which keeps calling this, ends the run on exitFlag.
// PBI requests core0_utilityTasks()'s poll handled because bmon never
// dispatched them (reported at the end of the run).
DRAM_ATTR int pbiPollCatches = 0;

#ifdef FAKE_SIO
DRAM_ATTR int fakeSioPokes = 0;   // reported at the end of the run
DRAM_ATTR uint8_t fakeSioResults = 0;   // the last request's result flags
#endif

#if CONFIG_A8BB_SIDECAR_ENABLE && !defined(CSIM)
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
    printf("Diagnostic mode (Sidecar, flags %02x): bus never attached; serving the link until reset\n",
           flags);
    fflush(stdout);
    if (flags & static_cast<uint8_t>(SidecarDiagnosticFlag::Panic)) {
        // Serve the link briefly first: the ack of the Sidecar's reply.
        const int64_t start = esp_timer_get_time();
        while (esp_timer_get_time() - start < 100000)
            sidecarHost.pollDiagnostic();
        printf("Diagnostic mode: panic requested by the Sidecar\n");
        fflush(stdout);
        abort();
    }
    if (flags & static_cast<uint8_t>(SidecarDiagnosticFlag::FetchFirmware)) {
        sidecarHost.installFirmware();   // restarts into the new app; back here only on failure
        printf("Diagnostic mode: OTA failed; serving the link until reset\n");
        fflush(stdout);
    }
    for (;;)
        sidecarHost.pollDiagnostic();   // serves Core (the core dump) too
}
#endif

static inline __attribute__((always_inline)) IRAM_ATTR bool core0_utilityTasks() {
    // No Sidecar link polling here (or anywhere in the bmon loop): main
    // serves Sidecar->main commands only in diagnostic mode, which is
    // decided at boot (sidecarDiagnosticMode()). Main->Sidecar commands
    // (SIO, CIO, Mount) are main's own calls.
#ifdef FAKE_SIO
    // Bench smoke test (FAKE_CLOCK, no 6502): twice a second, fill in the
    // PBI request block and the DCB the way the PBI ROM's SIO vector does
    // for a D1: sector 1 read, for the FAKE_CLOCK poll below to hand to
    // handlePbiRequest(): the SIO dispatch, the disk image read, serial
    // input (handleSerialIO()) and the copy-out decision all run. Nothing
    // checks the result. REQ_FLAG_NORMAL, not STACKWAIT: no 6502 will reach
    // the stack program, so waiting for it would end in "-3 stackprog
    // timeout".
    EVERYN_TICKS(120 * 1000 * 1000) {   // 0.5 s
        PbiIocb *fakeRequest = (PbiIocb *)&pbiROM[0x30];
        if (fakeRequest->req == 0) {
            if (fakeSioPokes > 0) fakeSioResults = fakeRequest->result;
            // handlePbiRequest2()'s first request maps the screen (SAVMSC,
            // 800 bytes) and asks the 6502 to copy in any unmapped part.
            // BENCH maps only $0000-$07FF as base memory, minus the stack
            // page, so the default SAVMSC of 0 would need a copy-in that
            // never comes: point it at $0400.
            atariRam[88] = 0x00;
            atariRam[89] = 0x04;
            AtariDCB *dcb = atariMem.dcb;
            dcb->DDEVIC = 0x31;   // D:
#if CONFIG_A8BB_SIDECAR_ENABLE
            dcb->DUNIT = 1 + (fakeSioPokes & 1);   // D1: the Sidecar's, D2: local (BENCH)
#else
            dcb->DUNIT = 1;
#endif
            dcb->DCOMND = 0x52;   // read sector
            dcb->DSTATS = 0x40;   // data from the drive
            dcb->DBUFLO = 0x00;   // $0600, in BENCH's base memory
            dcb->DBUFHI = 0x06;
            dcb->DBYTLO = 128;
            dcb->DBYTHI = 0;
            dcb->DAUX1 = 1;       // sector 1
            dcb->DAUX2 = 0;
            fakeRequest->cmd = 7;   // SIO, see the DCB
            fakeRequest->req = REQ_FLAG_NORMAL;
            fakeSioPokes++;
        }
    }
#endif
    // A PBI request normally reaches handlePbiRequest() through bmon: the
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
    if (bmonHead == bmonTail) {
        PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
        if (pbiRequest[0].req != 0) {
            pbiPollCatches++;
            handlePbiRequest(&pbiRequest[0]);
        } else if (pbiRequest[1].req != 0) {
            pbiPollCatches++;
            handlePbiRequest(&pbiRequest[1]);
        }
    }

    if (1) {
        static const DRAM_ATTR int keyTicks = 301 * 240 * 1000; // 150ms
        EVERYN_TICKS(keyTicks) { 
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
        elapsedSec++;
        if (1 && elapsedSec == 15 && ioCount > 0) {
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
    return false;
}

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
    // ends there (exit -6).
    bmonTail = bmonHead;
    mmuState.pinReleaseMask |= bus.halt_.mask;
#if !defined(CSIM) || defined(CSIM_BUS_CYCLE)   // csim: core 0 is a coroutine there
    {
#ifdef FAKE_CLOCK
        static const DRAM_ATTR uint64_t firstWriteTimeout = 240000000ULL;       // 1 s
#else
        static const DRAM_ATTR uint64_t firstWriteTimeout = 30 * 240000000ULL;  // 30 s
#endif
        uint32_t lastTsc = XTHAL_GET_CCOUNT();
        uint64_t waited = 0;   // CCOUNT wraps every 17.9 s
        for (int n = 0; n < 2; n++) {
            const unsigned int oldHead = bmonHead;
            while (oldHead == bmonHead && waited < firstWriteTimeout) {
                AsmNops<30>::generate();
                const uint32_t now = XTHAL_GET_CCOUNT();
                waited += now - lastTsc;
                lastTsc = now;
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
    while(!exitFlag) {
        uint32_t stsc = XTHAL_GET_CCOUNT();
        const static DRAM_ATTR uint32_t bmonTimeout = 240 * 1000 * 50;
	    while(XTHAL_GET_CCOUNT() - stsc < bmonTimeout) {
            CSIM_HOOK();
#if !defined(CSIM) || defined(CSIM_BUS_CYCLE)
            // Warning: this is a core0 ifetch hotspot, with a hand-tuned loop delay.
            while(
                XTHAL_GET_CCOUNT() - stsc < bmonTimeout && 
                bmonHead == bmonTail) {
#pragma GCC unroll 8
                for (int poll = 0; poll < 8 && bmonHead == bmonTail; ++poll) {
                    loopNezDelay(33);   // 30 passes + setup; was 15 two-cycle passes
                }
            }
#endif // CS
            if (bmonHead != bmonTail)
                core0_onBmonChange();
        }
        //restartHalted6502();
        if (core0_utilityTasks())
            break;
    }
}

void IFLASH_ATTR threadFunc(void *) { 
    printf("CORE0: threadFunc() start\n");

    printf("runSec %d\n", config.runSec);
    uint8_t chipid[6];
    esp_read_mac(chipid, ESP_MAC_WIFI_STA);
    mmuDebugPrint();
    printf("HW_TYPE %s\n", HW_TYPE);
    printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",chipid[0], chipid[1], chipid[2], chipid[3], chipid[4], chipid[5]);
    printf("GIT: " GIT_VERSION " \n");

    //XT_INTEXC_HOOK oldnmi = _xt_intexc_hooks[XCHAL_NMILEVEL];
    uint32_t oldint;

    portDISABLE_INTERRUPTS();
    disableCore0WDT();
    //_xt_intexc_hooks[XCHAL_NMILEVEL] = my_nmi; 
    //__asm__ __volatile__("rsil %0, 1" : "=r"(oldint) : );

    core0Loop();
#ifndef FAKE_CLOCK
    BmonEntry *bmonCopy = new BmonEntry[bmonArraySz];
    for(int i = 0; i < bmonArraySz; i++) { 
        bmonCopy[i].r0 = bmonArray[i].r0;
    }
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
#ifdef FAKE_CLOCK
    printf("Shutdown atariRam[0x0180]=%02x\n", atariRam[0x0180]);
#endif
    //_xt_intexc_hooks[XCHAL_NMILEVEL] = oldnmi;
    //__asm__("wsr %0,PS" : : "r"(oldint));
    printf("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", in0, in1);
    printf("GPIO_EN_REG: %08" PRIx32 " %08" PRIx32 "\n", en0, en1);
    printf("in1: ");
    if ((in1 & bus.extSel.mask) == 0) printf("(extsel_ AL)");
    if ((in1 & bus.halt_.mask) == 0) printf("(halt_ AL)");
    if ((in1 & bus.irq_.mask) == 0) printf("(irq_ AL)");
    if ((in1 & bus.mpd.mask) == 0) printf("(mpd_ AL)");
    printf("\n");
    printf("en1: ");
    if ((en1 & bus.extSel.mask) != 0) printf("(extsel_)");
    if ((en1 & bus.halt_.mask) != 0) printf("(halt_)");
    if ((en1 & bus.irq_.mask) != 0) printf("(irq_)");
    if ((en1 & bus.mpd.mask) != 0) printf("(mpd_)");
    printf("\n");
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    mmuDebugPrint();
#ifndef FAKE_CLOCK
    printf("bmonMax: %d mmuChangeBmonMaxEnd: %d mmuChangeBmonMaxStart: %d\n", bmonMax, mmuChangeBmonMaxEnd, mmuChangeBmonMaxStart);   
    printf("bmonArray:\n");
    uint16_t *addrHistogram = (uint16_t *)(uint8_t *)heap_caps_malloc(64 * 1024 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    printf("addrHistogram allocated %p\n", addrHistogram);
    if (addrHistogram != NULL) { 
        for(int n = 0; n < 64 * 1024; n++) addrHistogram[n] = 0;

        for(int i = 0; i < bmonArraySz; i++) { 
            uint32_t r0 = (bmonCopy[i].r0) >> bmonR0Shift;
            uint16_t addr = r0 >> bus.addr.shift;
            addrHistogram[addr]++;
 	
            uint8_t data = (bmonCopy[i].r0 & 0xff);
            if (bmonExclude(bmonCopy[i].r0)) continue;
            if (0) { // exhaustively print every bmon entry  
                char rw = (r0 & bus.rw.mask) != 0 ? 'R' : 'W';
#ifdef HAVE_REFRESH
                if ((r0 & bus.refresh_.mask) == 0) rw = 'F';
#endif
                    printf("%c %04x %02x\n", rw, addr, data);
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
            printf("bmon hotspot %d: addr=%04x count=%d\n", addrCount, (int)hotAddr, hotAddrCount);
            addrHistogram[hotAddr] = 0;
       }
    }
 #endif


#ifdef PROFILE_ENABLE_ARRAY
    profilePrint(opt.histogram, ramReads);
#endif
    
    printf("DUMP %.2f\n", millis() / 1000.0);
    
    if (opt.tcpSendPsram && psram != NULL) { 
        printf("TCP SEND %.2f\n", millis() / 1000.0);
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
                    printf("BT%7d us ", (int)(*(p + 1) - lastTrigger) / 240);
                    lastTrigger = *(p + 1);
                } else if (*p != 0) { 
                    printf("B            ");
                }
                uint32_t r0 = ((*p) >> bmonR0Shift);
                uint16_t addr = r0 >> bus.addr.shift;
                char rw = (r0 & bus.rw.mask) != 0 ? 'R' : 'W';
#ifdef HAVE_REFRESH
                if ((r0 & bus.refresh_.mask) != 0) rw = 'F';
#endif
                uint8_t data = (*p & 0xff);
                const char *op = ops[data];
                if (op == NULL) op = "";
                if (*p != 0) {
                    printf("%c %04x %02x   %s\n", rw, addr, data, op);
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
    printf("atariRam[754] = %d\n", atariRam[754]);
    printf("pbiROM[0x100] = %d\n", pbiROM[0x100]);
    printf("reg[0xd301] = 0x%02x\n", d000Write[0x301]);
    printf("reg[0xd1ff] = 0x%02x\n", d000Write[0x1ff]);
    printf("ioCount %d, interruptCount %d\n", ioCount, pbiInterruptCount);
    if (structLogs) structLogs->print();
    printf("Page 6: ");
    for(int i = 0x600; i < 0x620; i++) { 
        printf("%02x ", atariRam[i]);
    }
    printf("\nHATABS: ");
    for(int x = 0x031A; x <= 0x31a + 36 && atariRam[x] != 0; x += 3) { 
        printf("%c=%04x ", atariRam[x], atariRam[x + 1] + (atariRam[x + 2] << 8));
    }
    printf("\npbiROM:\n");
    for(int i = 0; i < min((int)sizeof(pbiROM), 0x60); i++) { 
        printf("%02x ", pbiROM[i]);
        if (i % 16 == 15) printf("\n");
    }
    printf("\nstack:\n");
    for(int i = 0; i < 256; i++) { 
        if (i % 16 == 0) printf("\n0x1%02x: ", i);
        printf("%02x ", atariRam[i + 0x100]);
    }
    printf("\ndisplay list:\n");
    uint16_t dlist = atariRam[560] + (atariRam[561] << 8);
    for(int i = 0; i < 32; i++) { 
        if (i % 8 == 0) printf("%04x: ", dlist + i);
        printf("%02x ", atariRam[dlist + i]);
        if (i % 8 == 7) printf("\n");
    }
    dumpScreenToSerial('B');
#endif
    
    int memReadErrors = 0;//(atariRam[0x609] << 24) + (atariRam[0x608] << 16) + (atariRam[0x607] << 16) + atariRam[0x606];
    printf("SUMMARY %-10.2f/%d e%d i%d d%d %s\n", millis()/1000.0, config.runSec, memReadErrors, 
        pbiInterruptCount, ioCount, exitReason.c_str());
    printf("PBI requests caught by the poll, not bmon: %d\n", pbiPollCatches);
#ifdef FAKE_SIO
    printf("FAKE_SIO: %d requests poked, %d I/Os handled, last result %02x, SAVMSC %04x\n",
           fakeSioPokes, ioCount, fakeSioResults, atariRam[88] | (atariRam[89] << 8));
#endif
    printf("pbi_init_complete %d, halts %d\n", pbiROM[0x20], haltCount);
    printf("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    printf("GPIO_EN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_ENABLE_REG),REG_READ(GPIO_ENABLE1_REG)); 
    printf("extMem swaps %d evictions %d d1ff %02x pinDr %08lx\n", 
        extMem.swapCount, extMem.evictCount, d000Write[0x1ff], 0L/*pinDriveMask*/);

    printf("lastPageWriteOffset[0xd5] %02x atariCart.bankA0 %02x l1Blocks[0xa0] %p cartBanks[1] %p atariCart.image[1].l1Block %p\n",
        lastPageOffset[pageD5], atariCart.bankA0, mmuState.l1Blocks[l1BlockA0], mmuState.cartBanks[1], atariCart.image != NULL ? &atariCart.image[1].l1Block : 0);
    printf("interruptTicks=%d,interruptRequested=%d,deferredInterrupt=%d\n", config.interruptTicks, interruptRequested, deferredInterrupt);
    printf("STACK HIGH-WATER: minimum unused / configured stack (bytes)\n");
    auto printStackHighWater = [](const char *name, TaskHandle_t task, size_t stackSizeBytes) {
        if (task == 0) {
            printf("STACK %-16s not running\n", name);
            return;
        }
        UBaseType_t freeBytes = uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t);
        printf("STACK %-16s %u/%u bytes free\n", name, (unsigned)freeBytes,
            (unsigned)stackSizeBytes);
    };
    auto printNamedTaskStackHighWater = [&printStackHighWater](const char *name, size_t stackSizeBytes) {
        printStackHighWater(name, xTaskGetHandle(name), stackSizeBytes);
    };
    printStackHighWater("core0Loop", xTaskGetCurrentTaskHandle(), 12 * 1024);
    printNamedTaskStackHighWater("main", ESP_TASK_MAIN_STACK);
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
#if CONFIG_A8BB_ENABLE_MDNS
    printNamedTaskStackHighWater("mdns", CONFIG_MDNS_TASK_STACK_SIZE);
#endif
    printf("DONE %-10.2f %s\n", millis() / 1000.0, exitReason.c_str());
    delay(100);
    
    //ESP.restart();
#ifndef CSIM
    printf("CORE0 idle\n");
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
        printf("  %s: %08x..%08x changed (%u of %u bytes)\n", when, (unsigned)(start + i),
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
            printf("ROM DRAM reclaim: core 1 not up after 1 s; keeping the span reserved\n");
            return 0;
        }
    }
    const size_t changed = romReclaimChanges("ROM core 1 boot", kRomReclaimPattern);
    if (changed) {
        printf("ROM DRAM reclaim: the ROM's core 1 boot wrote %u bytes outside .data_xtos; "
               "keeping the span reserved (see AGENTS.md)\n", (unsigned)changed);
        return 0;
    }
    size_t added = 0;
    for (const auto &part : kRomReclaimParts) {
        if (heap_caps_add_region(part.start, part.end) == ESP_OK) {
            added += part.end - part.start;
        } else {
            printf("ROM DRAM reclaim: heap_caps_add_region(%08x, %08x) failed\n",
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
    printf("ROM_BOOT_PROBE exercise: %u-byte block at %p (%s the reclaimed parts)\n",
           (unsigned)size, block, inPart ? "in" : "NOT in");
    heap_caps_free(block);
    printf("ROM_BOOT_PROBE heap integrity: %s\n",
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

void IFLASH_ATTR startCpu1() {  
    if (REG_GET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN)) {
        printf("APP CPU is already running!\n");
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
#if CONFIG_A8BB_ENABLE_MDNS
#include "mdns.h"
#endif

#if CONFIG_A8BB_ENABLE_MDNS
static char mdnsHostname[MDNS_NAME_BUF_LEN];

void startMdnsWithMacHostname() {
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    char macString[13];
    snprintf(macString, sizeof(macString), "%02X%02X%02X%02X%02X%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    string hostname = config.wifiHostname;
    size_t replacementPos = 0;
    while ((replacementPos = hostname.find("$MAC", replacementPos)) != string::npos) {
        hostname.replace(replacementPos, 4, macString);
        replacementPos += sizeof(macString) - 1;
    }
    if (hostname.length() >= sizeof(mdnsHostname)) {
        ESP_LOGE("mDNS", "Hostname is too long: %u characters", (unsigned)hostname.length());
        return;
    }
    snprintf(mdnsHostname, sizeof(mdnsHostname), "%s", hostname.c_str());

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(mdnsHostname));
    ESP_ERROR_CHECK(mdns_instance_name_set("Atari Bus ESP32"));
    printf("mDNS hostname: %s.local\n", mdnsHostname);
}
#endif

void setup() {
    if (setupComplete)
        return;
#ifdef ATARI800_SIM
    setupAtariRam = MEMORY_mem;
    setupAtariRamSize = 65536;
    atariRam = MEMORY_mem;
#endif
    for(auto i : gpios) pinMode(i, INPUT);
#ifdef PROFILE_GPIO_SAMPLE
    profileGpioInitCore0();
#endif
    pinMode(bus.halt_.pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(bus.halt_.pin, 0);
#ifndef BOOT_CONFIG
#define BOOT_CONFIG ""
#endif
    config.load(BOOT_CONFIG);
    if (config.startupDelayMs > 0) delay(config.startupDelayMs);
    if (config.haltAvailable == false) {
        mmuState.pinReleaseMask |= bus.halt_.mask;
    } else { 
        mmuState.pinReleaseMask &= bus.halt_.maskInverse;
    }
    baseMemSz = config.baseMemSz; 
    ESP_ERROR_CHECK(baseMemSz > 0 && baseMemSz % 1024 == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE);
    if (setupAtariRam != nullptr) {
        if (setupAtariRamSize < static_cast<size_t>(baseMemSz))
            abort();
        atariRam = setupAtariRam;
        baseMemSz = static_cast<int>(setupAtariRamSize);
    } else {
        // AUTO memory: atariRam is only the first 2K, which stays; mmuInit()
        // maps the rest in pieces that autoMemConfig() can free.
        const int atariRamSz = config.autoMem ? 2 * 1024 : baseMemSz;
        atariRam = (uint8_t *)heap_caps_aligned_alloc(16,
                                                      atariRamSz,
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        assert(atariRam != NULL);
        bzero(atariRam, atariRamSz);
    }
    atariMem.dcb = (AtariDCB *)&atariRam[0x300];
    atariMem.ziocb = (AtariIOCB *)&atariRam[0x20];
    atariMem.iocb0 = (AtariIOCB *)&atariRam[0x320];


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
        printf("OK %d %d\n", digitalRead(44), digitalRead(0));
    }
#endif
    if (0) { 
        for(auto p : gpios) pinMode(p, INPUT);
        pinDisable(bus.extDecode.pin);

        while(1) { 
            for(auto p : gpios) {
                printf("%02d:%d ", p, digitalRead(p));
            }
            printf("\n");
            delay(50);
        }
    }

    //for(auto i : gpios) pinMode(i, INPUT);

    usb_serial_jtag_driver_config_t jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&jtag_config);

    if (opt.testPins) { 
        for(auto p : gpios) pinMode(p, INPUT_PULLUP);
        while(1) { 
            for(auto p : gpios) {
                printf("%02d:%d ", p, digitalRead(p));
            }
            printf("\n");
            delay(200);
        }
    }
    
    if (!extMem.init(16, config.extMemSramBanks, config.extMemConf))
        printf("Extended memory unavailable; continuing without it\n");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = false
    };
    printf("mounting spiffs...\n");
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    while (ret != ESP_OK) {
        printf("Could not mount or format spiffs!\n");
        delay(500);
    } 
    printf("mounted\n");

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        printf("Failed to get SPIFFS partition information (%s)\n", esp_err_to_name(ret));
    } else {
        printf("Partition size: total: %zu, used: %zu\n", total, used);
    }

    psram = (uint32_t *) heap_caps_aligned_alloc(64, psram_sz,  MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    psram_end = psram + (psram_sz / sizeof(psram[0]));
    if (psram != NULL)
        bzero(psram, psram_sz);


    printf("cartImage='%s'\n", config.cartImage.c_str());
#if CONFIG_A8BB_SYSMONITOR
    sysMonitor = new SysMonitor();
#endif
    cioFiles = new CioDispatcher();
    // Off while soak testing the struct logs' removal (809b496) on test
    // devices 5 and 6; structLogsInit() turns them on.
    //structLogsInit();
    sio.setNetworkUp(wifiRun);
    sio.setProcFsNodes(&procFsNodes);
    // The drives are mounted below, after sidecarHost.begin().
    atariCart.open(sio.resolve(config.cartImage).c_str());
    if (atariCart.bankA0 >= 0) 
    	  pbiROM[DISABLE_BASIC - PBIROM_BASE] = 1;

#if 0 //ndef BOOT_SDX
    // 169572 before sdkconf changes
    // 174595 after malloc and malloc 0 changes
    // 91719 with wiFi 
    // 92207 after lwip and wifi changes

    connectWifi(); // 82876 bytes 
    //connectToServer();
    start_webserver();  //12516 bytes 
#endif

    while(0) { 
        yield();
        delay(500);
        printf("OK\n");
    }
    //for(auto i : gpios) pinMode(i, INPUT);
    while(opt.watchPins) { 
            delay(100);
            printf("PU   %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG));
    }

#ifdef FAKE_CLOCK
    setupFakeBusInjection();
#endif

    //pinDisable(bus.extDecode.pin);
    for(int i = 0; i < 1; i++) { 
        printf("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    }
    printf("freq %.4fMhz threshold %d halfcycle %d psram %p\n", 
        testFreq / 1000000.0, lateThresholdTicks, (int)halfCycleTicks, psram);

    esp_rom_gpio_connect_in_signal(bus.clock.pin, CORE1_GPIO_IN0_IDX, false);
#ifdef PROFILE_GPIO_SAMPLE
    profileGpioInitCore1();
#endif
    digitalWrite(bus.irq_.pin, 1);
    pinMode(bus.irq_.pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(bus.irq_.pin, 1);
    //initLed();
#if HAS_STATUS_LED
    led.write(0, 20, 0);
#endif

    pinMode(bus.irq_.pin, OUTPUT_OPEN_DRAIN);
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.irq_.mask);
    digitalWrite(bus.irq_.pin, 0);

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
        printf("dedicated GPIO clock was off\n");
    REG_SET_BIT(SYSTEM_CPU_PERI_CLK_EN_REG, SYSTEM_CLK_EN_DEDICATED_GPIO);
    REG_CLR_BIT(SYSTEM_CPU_PERI_RST_EN_REG, SYSTEM_RST_EN_DEDICATED_GPIO);
    esp_rom_gpio_connect_out_signal(bus.extSel.pin, CORE1_GPIO_OUT0_IDX + bus.ctrlExtSel.shift, true, false);
    REG_SET_BIT(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * bus.extSel.pin, GPIO_FUNC0_OEN_SEL);
    esp_rom_gpio_connect_out_signal(bus.mpd.pin, CORE1_GPIO_OUT0_IDX + bus.ctrlMpd.shift, true, false);
    REG_SET_BIT(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * bus.mpd.pin, GPIO_FUNC0_OEN_SEL);
#endif
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, bus.extSel.mask | bus.mpd.mask);
    printf("extSel_ and mpd_ on core 1 dedicated channels %d and %d\n",
           bus.ctrlExtSel.shift, bus.ctrlMpd.shift);

    pinMode(bus.halt_.pin, OUTPUT_OPEN_DRAIN);
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
#if CONFIG_A8BB_SIDECAR_ENABLE
    sidecarHost.begin();   // probes, sends the boot report, takes the Sidecar's reply
#ifndef CSIM
    if (sidecarHost.diagnostic())
        sidecarDiagnosticMode();   // never returns: the bus is never attached
#endif
#endif
    mountDiskSpecs(config.diskSpec);
    //if (sio.disk(2) == NULL)
    //    sio.mount(2, "smb://miner6.local/pub");
    //if (sio.disk(3) == NULL)
    //    sio.mount(3, "proc:");
    mmuDebugPrint();
    enableBus();
    // WiFi's driver memory now, before the heap report, the ROM DRAM
    // reclaim guard and core 1, and before AUTO memory
    // sizes its banks at PBI init; the radio start and connect stay deferred.
    if (config.enableWifi)
        wifiPreinit();
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    reportHotDataBanks();
#if !defined(CSIM) && CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
    romReclaimFill(kRomReclaimPattern);
#ifdef ROM_BOOT_PROBE
    memcpy(romXtosBefore, (const void *)ROM_XTOS_DATA_START, sizeof(romXtosBefore));
#endif
#endif
    startCpu1();
    busywait(.01);
#if !defined(CSIM) && CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
#ifdef ROM_BOOT_PROBE
    while (!appCpuRunning) {}
    printf("ROM_BOOT_PROBE: %u bytes of .data_xtos changed by core 1 boot\n",
           (unsigned)romSpanChanges(".data_xtos", ROM_XTOS_DATA_START, ROM_XTOS_DATA_END, 0,
                                    romXtosBefore));
    printf("ROM_BOOT_PROBE: %u bytes of the reclaimed parts changed by core 1 boot\n",
           (unsigned)romReclaimChanges("boot", kRomReclaimPattern));
    busywait(3);
    printf("ROM_BOOT_PROBE: %u bytes of the reclaimed parts changed 3 s later\n",
           (unsigned)romReclaimChanges("3 s", kRomReclaimPattern));
#endif
    {
        const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#if defined(ROM_BOOT_PROBE) && ROM_BOOT_PROBE == 4
        const size_t reclaimed = 0;   // the control: keep the span reserved
#else
        const size_t reclaimed = reclaimRomAppCpuBootDram();
#endif
        printf("Reclaimed %u bytes of ROM core 1 boot DRAM; internal heap free %u -> %u\n",
               (unsigned)reclaimed, (unsigned)internalBefore,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
#ifdef ROM_BOOT_PROBE
    romBootProbeExercise();
#if ROM_BOOT_PROBE >= 3
    // Core 1 faults 5 s after it starts: core 0 should panic about then.
    for (int i = 0; i < 4; ++i) {
        busywait(2);
        printf("ROM_BOOT_PROBE: %d s, no panic yet; heap integrity %s\n", 2 * (i + 1),
               heap_caps_check_integrity_all(true) ? "ok" : "CORRUPT");
    }
#endif
#if ROM_BOOT_PROBE == 2
    {
        static RTC_NOINIT_ATTR uint32_t probeAborts;
        if (esp_reset_reason() != ESP_RST_PANIC) probeAborts = 0;
        if (probeAborts++ == 0) {
            printf("ROM_BOOT_PROBE: abort() to exercise the panic path\n");
            abort();
        }
        printf("ROM_BOOT_PROBE: booted after the panic; not aborting again\n");
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
void csim_app_core0_pass();
#ifdef CSIM_BUS_CYCLE
void csim_core0_resume();   // csim/a800/csim_stubs.cpp: core 0 as a coroutine
#endif

// atari800's CPU tick (every 16 or more cycles, and each halted cycle):
// advance the time, then run core 0. Bus-cycle mode resumes core 0's
// coroutine, which runs the real threadFunc()/core0Loop() until it waits
// (a CCOUNT read every so often, a busy wait) and yields back; the default
// mode runs csim_app_core0_pass(), csim's own pieces of core 0, in line.
void csim_app_cpu_tick(uint32_t cycles) {
    if (!setupComplete)
        return;

    CSIM_ADVANCE_CCOUNT(cycles * 132U);
#ifdef CSIM_BUS_CYCLE
    csim_core0_resume();
#else
    csim_app_core0_pass();
#endif
}

// One pass of csim's core 0 work.
void csim_app_core0_pass() {
    const uint64_t now = CSIM_PEEK_CCOUNT64();
    elapsedSec = static_cast<int>(now / (240ULL * 1000000ULL));

    static uint64_t lastInterruptTsc = 0;
    if (config.interruptTicks > 0 && !interruptRequested && ioCount > 0
        && now - lastInterruptTsc > static_cast<uint64_t>(config.interruptTicks)) {
        lastInterruptTsc = now;
#ifdef CSIM_BUS_CYCLE
        // The board answers $D1FF reads itself here, from d000Read[]:
        // raise the interrupt as the hardware build does.
        raiseInterrupt();
#else
        interruptRequested = 1;
        atariRam[PDIMSK] |= pbiDeviceNumMask;
#endif
 
    }
#ifdef CSIM_BUS_CYCLE
    // Bus-cycle mode: core 1's cycles fill bmon; take them as core0Loop()
    // does (PBI requests, PORTB/$D1FF/$D5xx follow-ups).
    while (bmonHead != bmonTail)
        core0_onBmonChange();
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
        printf("DONE\nExit reason %d %s\n", exitFlag, exitReason.c_str());
        exit(0);
    }
}

#ifdef CSIM_BUS_CYCLE
// threadFunc() has returned on core 0's coroutine: the run is over.
void csimCore0Finish() {
    sio.reportStats();
    printf("Exit reason %d %s\n", exitFlag, exitReason.c_str());
    fflush(stdout);
    exit(0);
}
#endif

int csim_app_pbi_irq_pending(void) {
    return interruptRequested != 0;
}

void csim_app_setup_with_ram(uint8_t *ram, size_t ramSize) {
    if (setupComplete)
        return;
    setupAtariRam = ram;
    setupAtariRamSize = ramSize;
    atariRam = ram;
    setup();
}
#endif
