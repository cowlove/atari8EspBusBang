#ifdef CSIM
#include "csim.h"
#endif
#include "a8log.h"
#include "asmDefs.h"
#define PBIROM_SYMBOLS_ONLY
#include "pbirom.h"   // PBIROM_<label> addresses
#include "screenScrape.h"
#include "fakeSio.h"

#include <inttypes.h>
#include <esp_intr_alloc.h>
#include <rtc_wdt.h>
#include <esp_task_wdt.h>
#include <esp_async_memcpy.h>
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_reg.h"
#include <rtc_wdt.h>
#include <hal/cache_hal.h>
#include <hal/cache_ll.h>
#include <hal/dedic_gpio_cpu_ll.h>
#include <xtensa/core-macros.h>
#include <xtensa/hal.h>
#include <driver/dedic_gpio.h>
#include <xtensa_timer.h>
#include <xtensa_rtos.h>
#include "rom/ets_sys.h"
#include "soc/dport_access.h"
#include "soc/system_reg.h"
#include "esp_partition.h"
#include "soc/assist_debug_reg.h"
#include "esp_spiffs.h"
#include "esp_mac.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include <deque>
#include <functional>
#include <algorithm>
#include <inttypes.h>
#include "soc/soc_caps.h"

#include "main.h"
#include "pbi.h"
#include "hostSio.h"
#include "apt.h"
#include "sfmt.h"
#include "extMem.h"
#include "hostSio.h"
#include "cartridge.h"
#include "sysMonitor.h"
#include "sysConfig.h"
#include "ascii2keypress.h"
#include "cio.h"
#include "log.h"
#include "led.h"
#include "util.h"
#include "hotCacheTrap.h"
#include "telnetScreen.h"
#include "arduinoLite.h"
#if A8BB_SIDECAR
#include "sidecarHost.h"
#endif
#include "screenshot.h"   // screenshotRequested, screenshotToSidecar() (no-ops without the Sidecar)

using std::min;
using std::max;

void connectWifi();

#if defined(PROFILE_GPIO_SAMPLE) && !defined(FAKE_CLOCK)
void IRAM_ATTR profileBus() { 
        // PROFILE_MODE=GPIO: sample the live 6502 bus traffic and timing
        // for a short time (about 4 ms), right after each Sidecar call.
        static DRAM_ATTR uint32_t samplePeriodEsp32Ticks = 1 * 1000 * 1000;
        if (!Hist2::binsFull) 
            profileGpioSampleBurstForCycles(samplePeriodEsp32Ticks);
}
#define PROFILE_BUS() profileBus()
#else
#define PROFILE_BUS() {}
#endif

// Core 1 stalled (RUNSTALL) through every cold section (-DCOLD_CORE1_STALL=0:
// left running, as before 2026-11-28). Detached, it is on the transparent
// map, driving nothing and serving nothing: its bmon entries are discarded
// at the re-attach. Running, it broke the 130XE .5: SPI1 flash operations
// from the cold path (SPIFFS reads, esp_flash_read(), with their cache
// suspends) while core 1 ran its loop left the 6502 wedged soon after the
// re-attach, every boot; the same reads with core 1 stalled, or the same
// data read through the cache (mmap), were clean (SIO_FIRST_TEST 3-6). The
// mechanism is still unknown. Stalled after disableBus() and its settle (so
// it stops on the transparent map), released before the re-attach's 5000-
// tick settle. Noinline IRAM: SYSTEM_CORE_1_CONTROL_0_REG is a peripheral
// register, touched only at the transitions, the bus detached; declared to
// tools/verify_core0_hot_calls.py as cold calls (top-level CMakeLists.txt).
#ifndef COLD_CORE1_STALL
#define COLD_CORE1_STALL 1
#endif
#if COLD_CORE1_STALL && !defined(CSIM)
static IRAM_ATTR __attribute__((noinline)) void coldCore1Stall() {
    REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
}
static IRAM_ATTR __attribute__((noinline)) void coldCore1Release() {
    REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
}
#else
static inline void coldCore1Stall() {}
static inline void coldCore1Release() {}
#endif

struct ScopedInterruptEnable { 
    // TODO: make re-entrant: only perform enable-interrupts step if interrupts are 
    // currently masked, keep state and only disable interrupts in destructor if we
    // enabled them in the constructor
    uint32_t oldint;
    inline ScopedInterruptEnable() { 
        unmapCount++;
        disableBus();
        busyWait6502Ticks(20);
        coldCore1Stall();   // on the transparent map now (COLD_CORE1_STALL)
        hotCacheResume();   // HOT_CACHE_TRAP: cold from here
        enableCore0WDT();
        portENABLE_INTERRUPTS();
        yield();
    }
    inline ~ScopedInterruptEnable() {
        yield();
        portDISABLE_INTERRUPTS();
        //ASM("rsil %0, 15" : "=r"(oldint) : : );
        disableCore0WDT();
        hotCacheSuspend();   // HOT_CACHE_TRAP: hot again from the re-attach
        coldCore1Release();
        busyWait6502Ticks(5000); // wait for core1 to stabilize again 
        enableBus();
    }
};


bool IRAM_ATTR needSafeWait(PbiIocb *pbiRequest) {
    if ((pbiRequest->req & REQ_FLAG_DETACHSAFE) == 0) {
        pbiRequest->result |= RES_FLAG_NEED_DETACHSAFE;
        return true;
    } 
    return false;
}


volatile bool wifiInitialized = false;
void wifiRun() { 
#if !CONFIG_A8BB_WIFI
    return;   // built without WiFi (Kconfig A8BB_WIFI)
#else
    if (config.enableWifi == false)
        return;

    if (wifiInitialized == false) { 
        connectWifi(); // 82876 bytes; main's own smb: drives only
        wifiInitialized = true;
        for(int n = 0; n < Sio::kDrives; n++) {
            // TMP disable until better error handling 
            //if (sio.disk(n) != NULL) sio.disk(n)->start();
        }
    }
#endif   // CONFIG_A8BB_WIFI
}

struct ScopedBlinkLED { 
    DRAM_ATTR static uint8_t cur[3];// = {0};
    uint8_t prev[3];
    ScopedBlinkLED(uint8_t *set) {
        for(int n = 0; n < sizeof(cur); n++) prev[n] = cur[n];
        for(int n = 0; n < sizeof(cur); n++) cur[n] = set[n];
#if HAS_STATUS_LED
        led.write(set[0], set[1], set[2]); 
#endif
    }
    ~ScopedBlinkLED() {  
#if HAS_STATUS_LED
        led.write(prev[0], prev[1], prev[2]); 
#endif
        for(int n = 0; n < sizeof(cur); n++) cur[n] = prev[n];
    }
};
uint8_t ScopedBlinkLED::cur[3];
#ifdef CSIM
#define SCOPED_BLINK_LED(a,b,c) ((void)0)
#else
#define SCOPED_BLINK_LED(a,b,c) ScopedBlinkLED blink((uint8_t []){a,b,c});
#endif

void waitVblank(int offset) { 
    uint32_t vbTicks = 4005300;
    //int offset = 3700000;
    //int offset = 0;
    int window = 1000;
    while( // Vblank synch is hard hmmm          
        ((XTHAL_GET_CCOUNT() - lastVblankTsc) % vbTicks) > offset + window
        ||   
        ((XTHAL_GET_CCOUNT() - lastVblankTsc) % vbTicks) < offset
    ) {}
}

// One line of console input, from main's USB console or the Sidecar's
// ("main <line>" there, fetched by ConsoleIn below).
static void IFLASH_ATTR handleSerialLine(const char *line) {
    char x;
    if (sscanf(line, DRAM_STR("key %c"), &x) == 1) {
        putKey(x);
    } else if (sscanf(line, DRAM_STR("exit %c"), &x) == 1) {
        core0_exit(1, "3 User-requested halt '%c'", x);
    } else if (sscanf(line, DRAM_STR("screen %c"), &x) == 1) {
        dumpScreenToSerial(x);
#if A8BB_SIDECAR
    } else if (strcmp(line, "fetch test") == 0) {
        sidecarHost.fetchReport(SidecarObject::Test);
    } else if (strcmp(line, "fetch fw") == 0) {
        sidecarHost.fetchReport(SidecarObject::Firmware);
#endif
    }
}

void IFLASH_ATTR handleSerialInput() {
#ifndef GPIO19_IN_USE   // else no USJ driver (pinDefs.h): no USB console input
    uint8_t c;
    while(usb_serial_jtag_read_bytes((void *)&c, 1, 0) > 0) { 
        static DRAM_ATTR LineBuffer lb;
        lb.add(c, handleSerialLine);
    }
#endif
#if A8BB_SIDECAR && !A8BB_SIDECAR_LOOPBACK
    // The Sidecar's console input for main (tools/halt.sh, through the
    // Sidecar, on a board whose USB console is gone or out of reach): one
    // round trip per status line.
    Sidecar &sidecar = sidecarHost.sidecar();
    if (sidecar.present() && !sidecar.diagnostic()) {
        ConsoleInCommand command = {};
        ConsoleInResponse response;
        if (sidecar.consoleIn(command, response, 20000) == SidecarResult::Ok && response.body.length > 0) {
            static DRAM_ATTR LineBuffer sidecarLb;
            for (uint16_t i = 0; i < response.body.length; ++i)
                sidecarLb.add(static_cast<char>(response.data[i]), handleSerialLine);
        }
    }
#endif
}

void IFLASH_ATTR dumpScreenToSerial(char tag, uint8_t *mem/*= NULL*/) {
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    if (mem == NULL) {
        mem = mmuCheckRangeMapped(mmuStateSaved, savmsc, 24 * 40);
        if (mem == NULL) {
            LOG(DRAM_STR("SCREEN%c 00 memory at SAVMSC(%04x) not mapped, no screendump\n"), tag, savmsc);
            return;
        }
    }

    LOG(DRAM_STR("SCREEN%c 00 memory at SAVMSC(%04x):\n"), tag, savmsc);
    LOG(DRAM_STR("SCREEN%c 01 +----------------------------------------+\n"), tag);
    for(int row = 0; row < 24; row++) { 
        LOG(DRAM_STR("SCREEN%c %02d |"), tag, row + 2);
        for(int col = 0; col < 40; col++) { 
            uint8_t c = *(mem + row * 40 + col);
            char buf[16];
            screenMemToAscii(buf, sizeof(buf), c);
            LOG("%s", buf);
        }
        LOG(DRAM_STR("|\n"));
    }
    LOG(DRAM_STR("SCREEN%c 27 +----------------------------------------+\n"), tag);
    mmuDebugPrintMmuState(mmuStateSaved);
}

uint8_t *mappedElseCopyIn(PbiIocb *pbiRequest, uint16_t addr, uint16_t len) { 
    uint8_t *rval = mmuCheckRangeMapped(mmuStateSaved, addr, len);
#ifdef CSIM
    // for csim code coverage, force half IO's to use the copyin path
    static bool alternate = false;
    if (alternate) rval = NULL;
    alternate = !alternate;
#endif
    if (rval != NULL)
        return rval;
    
    if ((pbiRequest->req & REQ_FLAG_COPYIN) == 0 ||
        pbiRequest->copybuf != addr || pbiRequest->copylen < len) {
        // TODO assert(len < REQ_MAX_COPYLEN)
        pbiRequest->copybuf = addr;
        pbiRequest->copylen = len;
        pbiRequest->result |= RES_FLAG_NEED_COPYIN;
        return NULL;
    }
    return &pbiROM[0x400];
}    

IRAM_ATTR uint8_t *mappedElseCopyIn(MmuState &ms, PbiIocb *pbiRequest, uint16_t addr, uint16_t len,
                                    uint16_t *run, bool allCopied) {
    // A copy-in that holds this whole range wins, even over mapped pages: a
    // ROM may send a buffer pre-copied (its data in COPYBUF, the buffer
    // itself not to be trusted), and after our own copy-in of a remainder the
    // re-issued request finds it here.
    if ((pbiRequest->req & REQ_FLAG_COPYIN) != 0 && pbiRequest->copybuf <= addr &&
        uint32_t(addr) + len <= uint32_t(pbiRequest->copybuf) + pbiRequest->copylen &&
        uint32_t(addr - pbiRequest->copybuf) + len <= REQ_MAX_COPYLEN) {
        *run = len;
        return &pbiROM[0x400 + (addr - pbiRequest->copybuf)];
    }
    if (!allCopied) {
        if (uint8_t *mem = mmuMappedRun(ms, addr, len, run))
            return mem;
    }
    pbiRequest->copybuf = addr;
    pbiRequest->copylen = len;
    pbiRequest->result |= RES_FLAG_NEED_COPYIN;
    *run = 0;
    return nullptr;
}

IRAM_ATTR bool pbiGather(MmuState &ms, PbiIocb *pbiRequest, uint16_t addr, uint16_t len, uint8_t *dst,
                         bool allCopied) {
    for (uint16_t off = 0; off < len; ) {
        uint16_t run;
        const uint8_t *src = mappedElseCopyIn(ms, pbiRequest, addr + off, len - off, &run, allCopied);
        if (src == nullptr)
            return false;   // NEED_COPYIN for [addr + off, addr + len)
        memcpy(dst + off, src, run);
        off += run;
    }
    return true;
}

IRAM_ATTR int pbiScatter(MmuState &ms, PbiIocb *pbiRequest, uint16_t addr, uint16_t len, const uint8_t *src,
                         bool allCopied) {
    uint16_t off = 0;
    while (!allCopied && off < len) {
        uint16_t run;
        uint8_t *dst = mmuMappedRun(ms, addr + off, len - off, &run);
        if (dst == nullptr)
            break;
        memcpy(dst, src + off, run);
        off += run;
    }
    if (off == len)
        return RES_FLAG_COMPLETE;
    // The rest, from the first page the board doesn't map, through COPYBUF:
    // the ROM's copy-out writes it with the 6502's own stores, which reach
    // any mapped pages after that one too.
    pbiRequest->copybuf = addr + off;
    pbiRequest->copylen = len - off;
    memcpy(&pbiROM[0x400], src + off, len - off);
    return RES_FLAG_COPYOUT;
}

// Called from a pbiRequest context to copy in memory from 6502 native ram. 
//   Sets REQ_FLAG_COPYIN and returns false until successive pbi requests 
//   have completed the transfer, then finally returns true
bool pbiReqCopyIn(PbiIocb *pbiRequest, uint16_t start, uint16_t len, uint8_t *mem) {    
    if ((pbiRequest->req & REQ_FLAG_COPYIN) == 0) {
        // first call, initialize pbiRequest structure for copyin
        pbiRequest->copybuf = start;
        pbiRequest->copylen = min(REQ_MAX_COPYLEN, (int)len);
        pbiRequest->result |= RES_FLAG_NEED_COPYIN;
        return false;
    }

    int offset = pbiRequest->copybuf - start;
    for(int i = 0; i < pbiRequest->copylen; i++) 
        *(mem + offset + i) = pbiROM[0x400 + i];

    if (offset + pbiRequest->copylen < len) {
        pbiRequest->copybuf += pbiRequest->copylen;
        pbiRequest->copylen = min(REQ_MAX_COPYLEN, (int)len - offset);
        return false;
    }
    return true;
}

// called from a pbi command context to copy the data currently in native 6502 pages 
//   Sets REQ_FLAG_COPYIN and returns false until successive pbi requests 
//   have completed the transfer, then finally returns true

bool pbiCopyAndMapPages(PbiIocb *p, int startPage, int pages, uint8_t *mem) {
    if (!pbiReqCopyIn(p, startPage * pageSize, pages * pageSize, mem))
        return false;
    mmuMapRangeRW(startPage * pageSize, (startPage + pages) * pageSize - 1, mem);
    return true;
}

// called from a pbi command context to copy the data currently in native 6502 pages 
// into esp32 memory and map them.  
//   Sets REQ_FLAG_COPYIN and returns false until successive pbi requests 
//   have completed the transfer, then finally returns true

bool pbiCopyAndMapPagesIntoBasemem(PbiIocb *p, int startPage, int pages, uint8_t *mem) {
    if (!pbiCopyAndMapPages(p, startPage, pages, mem))
        return false;
    mmuAddBaseRam(startPage * pageSize, (startPage + pages) * pageSize - 1, mem);
    return true;           
}

// In flash: cold (handlePbiRequestCold(), the bus detached; it already calls
// flash code, WiFi and FreeRTOS), and its ~530 bytes of IRAM were the
// margin that keeps core 0's interrupt stack out of core 1's hot-data bank
// (the post-link check, tools/verify_core1_hot_data.py).
void handleSerialIO(PbiIocb *pbiRequest) { 
    if (0 && (pbiRequest->result & (RES_FLAG_NEED_COPYIN | RES_FLAG_COPYOUT)) != 0) { 
        LOG("copy in/out result=0x%02x, addr 0x%04x len %d\n", 
            pbiRequest->result, pbiRequest->copybuf, pbiRequest->copylen);     
    }
    if (config.logScreen) {
        DRAM_ATTR static int lastScreenLogSample = -1;
        if (elapsedSec != lastScreenLogSample) {
            logNewAtariScreenLines();
            lastScreenLogSample = elapsedSec;
            fflush(stdout);
        }
    }
#ifdef FAKE_SIO
    fakeSioFlush();   // what fakeSioPoll() queued: printing is safe here
#endif
    DRAM_ATTR static int lastPrint = -999;
    if (elapsedSec - lastPrint >= config.serialStatusSec) {
        handleSerialInput();
        lastPrint = elapsedSec;
        DRAM_ATTR static int lastIoCount = 0;
        LOG(DRAM_STR("%02d:%02d:%02d io: %8d (%3d) irq %d "
            "halt %d evict %d/%d"),
            elapsedSec/3600, (elapsedSec/60)%60, elapsedSec%60, ioCount,  
            ioCount - lastIoCount, 
            pbiInterruptCount, haltCount, extMem.evictCount, extMem.swapCount);
#if A8BB_SIDECAR
        // Sidecar link reads: NEW payloads and faulted reads, total (since
        // the last line), with the faults broken down when there are new ones.
        // TIMEOUT is not a fault: a healthy link with nothing new.
        if (sidecarHost.sidecar().link()) {
            DRAM_ATTR static i2slink_stats_t lastLink = {};
            i2slink_stats_t link;
            i2slink_stats(sidecarHost.sidecar().link(), &link);
            auto faults = [](const i2slink_stats_t &s) {
                return s.reads_torn + s.reads_crc + s.reads_no_lock + s.reads_stalled;
            };
            const uint32_t newFaults = faults(link) - faults(lastLink);
            LOG(DRAM_STR(" sidecar new %u (%u) err %u (%u)"),
                   (unsigned)link.reads_new, (unsigned)(link.reads_new - lastLink.reads_new),
                   (unsigned)faults(link), (unsigned)newFaults);
            LOG(DRAM_STR(" sio %u fail %u locks %u fb %u"), (unsigned)sidecarHost.sioRequests(),
                   (unsigned)sidecarHost.sioFailures(), (unsigned)link.locks,
                   (unsigned)link.locks_fallback);
            if (sidecarHost.sioRequests())
                LOG(DRAM_STR(" rt %u/%u us"),   // SIO round trip: mean/max
                    (unsigned)(sidecarHost.sioTicks() / sidecarHost.sioRequests() / 240U),
                    (unsigned)(sidecarHost.sioTicksMax() / 240U));
            LOG(DRAM_STR(" poll %u/%u max %u us"),   // bmon-loop polls: served/total, slowest
                (unsigned)sidecarHost.pollsServed, (unsigned)sidecarHost.polls,
                (unsigned)(sidecarHost.pollTicksMax / 240U));
            // Where the polls' time goes: how many found a frame worth
            // reading (a true hint(): a full read follows, up to a beam
            // window's wait), and every poll by its duration.
            LOG(DRAM_STR(" (%u read; us <4/4-15/16-63/64-255/256+ %u/%u/%u/%u/%u)"),
                (unsigned)sidecarHost.sidecar().hintReads(),
                (unsigned)sidecarHost.pollUs[0], (unsigned)sidecarHost.pollUs[1],
                (unsigned)sidecarHost.pollUs[2], (unsigned)sidecarHost.pollUs[3],
                (unsigned)sidecarHost.pollUs[4]);
            if (newFaults) {
                LOG(DRAM_STR(" [torn %u crc %u nolock %u stalled %u]"),
                       (unsigned)(link.reads_torn - lastLink.reads_torn),
                       (unsigned)(link.reads_crc - lastLink.reads_crc),
                       (unsigned)(link.reads_no_lock - lastLink.reads_no_lock),
                       (unsigned)(link.reads_stalled - lastLink.reads_stalled));
            }
            lastLink = link;
        }
#endif
        LOG(DRAM_STR("\n"));
#if BMON_DELAY_LOG
        // -DBMON_DELAY_LOG=1: the bmon rings' delay profile, a line of its
        // own after the io: line. Since the last line: the high-water marks
        // (write/read ring, entries waiting) and laps. The laps' totals stay
        // in bmonLaps/bmonRdLaps (the exit summary).
        DRAM_ATTR static unsigned int lastLaps = 0, lastRdLaps = 0;
        LOG(DRAM_STR("bmon: hwm %u/%u laps %u/%u"), bmonWrHwm, bmonRdHwm,
            bmonLaps - lastLaps, bmonRdLaps - lastRdLaps);
        lastLaps = bmonLaps; lastRdLaps = bmonRdLaps;
        // Each maximum's source (bmon.h): the phase core 0 was in, its
        // address, the microseconds since that ring's previous drain; then
        // the drains by depth found (1/2-3/4-7/8-15/16-31/32+).
        static const DRAM_ATTR char phaseNames[PH_COUNT][8] = {
            "loop", "write", "reads", "util", "key", "sec", "irq", "pbi", "fakesio", "poll"};
        const BmonMaxTag &w = bmonWrMaxTag, &r = bmonRdMaxTag;
        LOG(DRAM_STR(" (max w %s %04x %uus, r %s %04x %uus) depths w %u/%u/%u/%u/%u/%u r %u/%u/%u/%u/%u/%u"),
            phaseNames[w.phase < PH_COUNT ? w.phase : 0], (unsigned)w.addr, (unsigned)(w.gapCycles / 240U),
            phaseNames[r.phase < PH_COUNT ? r.phase : 0], (unsigned)r.addr, (unsigned)(r.gapCycles / 240U),
            (unsigned)bmonWrDepths[0], (unsigned)bmonWrDepths[1], (unsigned)bmonWrDepths[2],
            (unsigned)bmonWrDepths[3], (unsigned)bmonWrDepths[4], (unsigned)bmonWrDepths[5],
            (unsigned)bmonRdDepths[0], (unsigned)bmonRdDepths[1], (unsigned)bmonRdDepths[2],
            (unsigned)bmonRdDepths[3], (unsigned)bmonRdDepths[4], (unsigned)bmonRdDepths[5]);
#ifdef FAKE_SIO
        DRAM_ATTR static unsigned int lastFakeLaps = 0, lastFakeRdLaps = 0;
        LOG(DRAM_STR(" (fake %u/%u laps %u/%u)"), bmonFakeWrHwm, bmonFakeRdHwm,
            bmonFakeLaps - lastFakeLaps, bmonFakeRdLaps - lastFakeRdLaps);
        lastFakeLaps = bmonFakeLaps; lastFakeRdLaps = bmonFakeRdLaps;
#endif
        LOG(DRAM_STR("\n"));
#endif  // BMON_DELAY_LOG
        bmonStatsClear();
#ifdef FAKE_SIO
        bmonFakeWrHwm = bmonFakeRdHwm = 0;
#endif
        fflush(stdout);
        if (BUS_ANALYZER && elapsedSec > 12) { 
            LOG("DONE\n");
        }
        fflush(stdout);
        lastIoCount = ioCount;
    }
    if (config.serialScreenDumpSec > 0 &&
        elapsedSec - lastScreenShot >= config.serialScreenDumpSec) {
        handleSerialInput();
        dumpScreenToSerial('Y');
        fflush(stdout);
        lastScreenShot = elapsedSec;
    }
}

// Halt line: not tested, inferred from the memory probe (user,
// 2026-11-12). The board's halt_ drives PBI READY: the 6502's RDY on an XL
// (a clean pause), but through the PBI->ECI adapter a 130XE's ECI HALT:
// ANTIC's HALT output to the 6502C, which the XE's glue logic also latches
// to time ANTIC's cycles; the ECI has no RDY (docs/memory-autodetect.md,
// "Halt line"). It stops the 6502 and floats its bus: not usable. A test can't tell
// the two apart (both stop), so: no native banks (an XL or 600XL) = usable,
// native banks (an XE) = not. An 800XL with an XE-style RAM upgrade is
// taken for an XE (SRAM-only banks: slower, safe).
DRAM_ATTR int haltLineDetected = -1;   // -1 unprobed, 0 not usable, 1 usable

// The native memory probe (docs/memory-autodetect.md), a sketch: the
// overlay at $DC00 (pbirom.asm) brackets its probe with these two
// commands. Both run with the bus detached (inside handlePbiRequestHot()'s ScopedInterruptEnable),
// so the probe map goes into mmuStateSaved, which enableBus() installs on
// the way out. Results are printed only; nothing is reconfigured yet.
// The overlay is loaded once, with pbiROM[] at boot; SIO copies overwrite
// COPYBUF later, so a second PBI init (warm reset, Atari reboot) finds it
// gone. Reloading it is left for the bring-up/reset design
// (docs/memory-autodetect.md).
DRAM_ATTR bool memProbeActive = false;    // mmuOnChange() leaves the banks alone
DRAM_ATTR int memProbeTopKb = -1;         // native RAM top (16, 48 ...), -1 unprobed
DRAM_ATTR int memProbeNativeBanks = 0;    // distinct native 16K banks at $4000
static MmuState *memProbeSaved = nullptr; // the real map, during the probe

static void memProbeBegin() {
    if (memProbeSaved == nullptr)
        memProbeSaved = (MmuState *)heap_caps_malloc(sizeof(MmuState), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (memProbeSaved == nullptr) {
        LOG("memory probe: no RAM for the saved map; not probing\n");
        return;
    }
    memProbeSaved->safeCopy(mmuStateSaved);
    // Board RAM stays at $0000-$3FFF, and $C000-$FFFF (OS ROM, I/O, this
    // PBI window) as it is. $4000-$BFFF goes to the dummy block: no
    // extSel_, so native RAM, if any, answers. Core 1 picks $4000 from
    // extBanks[] and $8000 from basicEnL1BlockMux[]/cartBanks[], so those
    // go too.
    L1Block *const d = (L1Block *)&dummyL1Block;
    mmuStateSaved.l1Blocks[l1Block40] = d;
    mmuStateSaved.l1Blocks[l1Block80] = d;
    for (auto &b : mmuStateSaved.extBanks) b = d;
    for (auto &b : mmuStateSaved.basicEnL1BlockMux) b = d;
    for (auto &b : mmuStateSaved.cartBanks) b = d;
    memProbeActive = true;
}

// AUTO memory (docs/memory-autodetect.md), from the probe's findings, at
// PROBE_REPORT: the bus is detached, so everything lands in mmuStateSaved
// (and the shared L1Blocks), which enableBus() installs on the way out.
//  1. Base RAM: native RAM to 48K or more: free the boot map's pieces above
//     minBaseRam (keeping $D800-$DFFF, as a 2K boot has); otherwise (a
//     600XL) keep all 64K.
//  2. Extended banks: ExtBankPool AUTO with an SRAM budget that leaves
//     minFreeSram of internal RAM free, the halt line and the native banks.
//  3. The halt line: config.haltAvailable, halt_ in pinReleaseMask and the
//     PORTB-write halt follow the probe.
static void autoMemConfig(int minBaseRam, int minFreeSram) {
    const bool halt = haltLineDetected == 1;
    if (memProbeTopKb >= 48 && minBaseRam < 64 * 1024) {
        mmuFreeUnmapBaseRam(minBaseRam, 0xd7ff);
        mmuFreeUnmapBaseRam(0xe000, 0xffff);
        baseMemSz = minBaseRam;
    }
    const int freeSram = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // The bank blocks come out of internal RAM after the banks:
    // mmuBuildBankBlocks() gives each mapped bank select its own L1Block
    // (768 bytes; core 1 hot data), so reserve them first: otherwise up to
    // ~12 KB of minFreeSram goes to them. The count is the plan's for the
    // largest budget, an upper bound: with the halt line and PSRAM every
    // budget above 0 maps the same selects; without them (all banks in
    // SRAM) a smaller budget may map fewer, and this over-reserves a little.
    const bool psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= size_t(13 * 16 * 1024);
    const ExtMemAutoPlan worst = extMemPlanAuto(16, halt, psram, memProbeNativeBanks);
    int mappedSelects = 0;
    for (int b : worst.premap) mappedSelects += b >= 0;
    const int blockCost = mappedSelects * (int)sizeof(L1Block);
    const int spareSram = freeSram - minFreeSram - blockCost;
    int budget = spareSram > 0 ? spareSram / (16 * 1024) : 0;
    // A bank needs 16K in one piece: back off if the heap can't give them.
    while (!extMem.init(16, budget, ExtBankPool::AUTO, halt, memProbeNativeBanks) && budget > 0)
        budget--;
    mmuBuildBankBlocks(*(MmuState *)&mmuStateSaved);
    config.haltAvailable = halt;
    if (halt) mmuState.pinReleaseMask &= bus.halt_.maskInverse;
    else      mmuState.pinReleaseMask |= bus.halt_.mask;
    mmuSetPortbHalt(halt);
    LOG("AUTO memory: base RAM %dK, %d SRAM bank budget (%d free, keeping %d, bank blocks %d), halt line %s; "
           "internal RAM free now %d\n", baseMemSz / 1024, budget, freeSram, minFreeSram, blockCost,
           halt ? "used" : "absent", (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static void memProbeReport() {
    const uint8_t *ram = (const uint8_t *)&pbiROM[PBIROM_PROBE_RAM - 0xd800];
    const uint8_t *banks = (const uint8_t *)&pbiROM[PBIROM_PROBE_BANKS - 0xd800];
    if (!memProbeActive) {
        LOG("memory probe: report without a probe map; ignored\n");
        return;
    }
    // Native RAM: the 1 KB blocks above $4000 that held both patterns.
    // Reported in two short lines: a burst of long lines here (the bank
    // groups) never reached the hardware console.
    int topKb = 16;
    while (topKb < 48 && ram[topKb - 16]) topKb++;
    char map[33];
    for (int i = 0; i < 32; i++) map[i] = ram[i] ? 'R' : '.';
    map[32] = 0;
    memProbeTopKb = topKb;
    memProbeNativeBanks = 0;
    if (ram[0]) {
        // Distinct native banks: the readbacks under the CPU-bank PORTB
        // values (bit 4 low; bit 7 high, self-test off) that aren't main
        // memory's (read under bit 4 high).
        const uint8_t mainSig = banks[32 | 0b00100];
        bool seen[256] = {};
        for (int v = 32; v < 64; v++)
            if ((v & 0b00100) == 0 && banks[v] != mainSig && !seen[banks[v]]) {
                seen[banks[v]] = true;
                memProbeNativeBanks++;
            }
    }
    haltLineDetected = memProbeNativeBanks == 0 ? 1 : 0;
    LOG("memory probe: RAM to %dK (%s), %d native banks: %s, halt %s\n", topKb, map,
           memProbeNativeBanks, memProbeNativeBanks ? "an XE" : "an XL",
           haltLineDetected ? "usable" : "not usable (ECI)");
    fflush(stdout);
    delay(20);   // these lines were lost on the hardware console without it
    LOG("memory probe: $4000 under PORTB (v<<2)|3, v=32..63:");
    for (int v = 32; v < 64; v++) LOG(" %02x", banks[v]);
    LOG("\n");
    fflush(stdout);
    delay(20);
    mmuStateSaved.safeCopy(*memProbeSaved);
    heap_caps_free(memProbeSaved);
    memProbeSaved = nullptr;
    memProbeActive = false;
    if (config.autoMem)
        autoMemConfig(config.autoMinBaseRam, config.autoMinFreeSram);
}

// The PBI request's cold part: handlePbiRequestHot() calls it with the bus
// detached (its ScopedInterruptEnable), interrupts on, so it lives in
// flash and may call anything. It returns before the bus is re-attached.
void safeRebootParked();   // main.cpp
extern volatile int rebootRequested;   // below

static int handlePbiRequestColdBody(PbiIocb *pbiRequest);
#if defined(SIO_FIRST_TEST) && SIO_FIRST_TEST >= 7 && !defined(CSIM)
// SIO_FIRST_TEST 7 and 8 (below): core 1's health while detached, from core
// 0 (cold, the bus detached, the 6502 in the stack wait). Over 10 ms: the
// bmon heads' moves (reads, and writes: the wait writes nothing, so any is
// a phantom), then the last 240 read entries ($0104-$0106, the BCS loop's
// three, against ANTIC's DMA and anything else), then 1000 samples of core
// 1's PC from the assist-debug record (all should be in iloop_pbi), the
// record turned off again after.
extern "C" char _iloop_pbi_start[], _iloop_pbi_end[];
static void core1Probe(const char *label) {
    const unsigned rh0 = bmonRdHead, wh0 = bmonHead;
    const uint32_t t0 = XTHAL_GET_CCOUNT();
    while (XTHAL_GET_CCOUNT() - t0 < 240u * 10000u) {}
    const unsigned rh1 = bmonRdHead, wh1 = bmonHead;
    int stack = 0, other = 0;
    uint16_t others[4] = {};
    for (unsigned k = 1; k <= 240; k++) {
        const uint32_t r0 = bmonRdArray[(rh1 - k) & bmonRdArraySzMask].r0 >> bmonR0Shift;
        const uint16_t a = r0ToAddr(r0);
        if (a >= 0x104 && a <= 0x106) stack++;
        else if (other < 4) others[other++] = a;
        else other++;
    }
    REG_SET_BIT(SYSTEM_CPU_PERI_CLK_EN_REG, SYSTEM_CLK_EN_ASSIST_DEBUG);
    REG_CLR_BIT(SYSTEM_CPU_PERI_RST_EN_REG, SYSTEM_RST_EN_ASSIST_DEBUG);
    REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_PDEBUGENABLE_REG, 1);
    REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_RECORDING_REG, 1);
    int outside = 0;
    uint32_t firstOut = 0;
    for (int i = 0; i < 1000; i++) {
        const uint32_t t = XTHAL_GET_CCOUNT();
        while (XTHAL_GET_CCOUNT() - t < 240u) {}
        const uint32_t pc = REG_READ(ASSIST_DEBUG_CORE_1_RCD_PDEBUGPC_REG);
        if (pc < (uint32_t)_iloop_pbi_start || pc >= (uint32_t)_iloop_pbi_end) {
            if (outside++ == 0) firstOut = pc;
        }
    }
    REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_RECORDING_REG, 0);
    REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_PDEBUGENABLE_REG, 0);
    LOG("SIO_FIRST_TEST %d core1 %s: 10 ms: %u reads %u writes; last 240 reads: %d stack loop, %d other "
        "(%04x %04x %04x %04x); PC outside iloop_pbi %d/1000 (first %08x)\n",
        SIO_FIRST_TEST, label, rh1 - rh0, wh1 - wh0, stack, other, others[0], others[1], others[2],
        others[3], outside, (unsigned)firstOut);
}
// The burst: 400 ms of esp_partition_read()s of the spiffs partition (SPI1
// flash operations, no SPIFFS) in 256-byte pieces (SPIFFS's page size, like
// its reads: SIO_FIRST_TEST 3's 184 KB SPIFFS burst took ~380 ms and failed;
// 184 KB in 4 KB pieces, 17 ms, passed) into internal RAM, wrapping at
// 184 KB; core 1 stalled through it for 8. Each piece timed (CCOUNT).
// SIO_FIRST_TEST 9: the burst watched. After each read, the read ring's new
// entries: the first one off the stack loop ($0104-$0106) snapshots the last
// 64 read entries (raw GPIO_IN words) and the writes since the burst began;
// and GPIO_IN1 sampled, counting halt_ (35), irq_ (37), mpd_ (46) and
// extSel_ (47) low, against 1000 samples before the burst.
struct BurstWatch {
    static constexpr int kPins = 4;
    static constexpr int kPinBits[kPins] = {35 - 32, 37 - 32, 46 - 32, 47 - 32};
    uint32_t lowBefore[kPins] = {}, lowDuring[kPins] = {}, samples = 0;
    unsigned firstLowOp[kPins] = {}, firstLowUs[kPins] = {};
    unsigned rdTail = 0, wrStart = 0, offStack = 0, departOp = 0, departUs = 0, writes = 0, firstAt = 0;
    bool departed = false;
    uint32_t rd[64] = {}, wr[16] = {};
    void sample(uint32_t (&low)[kPins]) {
        const uint32_t g = REG_READ(GPIO_IN1_REG);
        for (int i = 0; i < kPins; i++) low[i] += ((g >> kPinBits[i]) & 1) == 0;
    }
    void begin() {
        for (int i = 0; i < 1000; i++) {
            sample(lowBefore);
            const uint32_t t = XTHAL_GET_CCOUNT();
            while (XTHAL_GET_CCOUNT() - t < 240u * 40u) {}   // ~40 ms in all
        }
        rdTail = bmonRdHead;
        wrStart = bmonHead;
    }
    // Left the loop: a read's worth of new entries (~80 bus cycles) with no
    // stack loop read at all (ANTIC's display DMA takes at most about half).
    void check(unsigned op, uint32_t us) {
        const uint32_t before[kPins] = {lowDuring[0], lowDuring[1], lowDuring[2], lowDuring[3]};
        sample(lowDuring);
        samples++;
        for (int i = 0; i < kPins; i++)
            if (lowDuring[i] == 1 && before[i] == 0) { firstLowOp[i] = op; firstLowUs[i] = us; }
        const unsigned head = bmonRdHead;
        if (head - rdTail > bmonRdArraySz) rdTail = head - bmonRdArraySz;
        const unsigned n = head - rdTail;
        unsigned stack = 0;
        for (; rdTail != head; rdTail++) {
            const uint16_t a = r0ToAddr(bmonRdArray[rdTail & bmonRdArraySzMask].r0 >> bmonR0Shift);
            if (a >= 0x104 && a <= 0x106) stack++;
            else offStack++;
        }
        if (!departed && n >= 32 && stack == 0) {
            departed = true;
            departOp = op;
            departUs = us;
            firstAt = n >= 64 ? 0 : 64 - n;   // where this op's entries start in rd[]
            for (int k = 0; k < 64; k++) rd[k] = bmonRdArray[(head - 64 + k) & bmonRdArraySzMask].r0;
            writes = bmonHead - wrStart;
            for (unsigned k = 0; k < 16 && k < writes; k++) wr[k] = bmonArray[(wrStart + k) & bmonArraySzMask].r0;
        }
    }
    void report() {
        LOG("SIO_FIRST_TEST 9: GPIO_IN1 low (halt_ irq_ mpd_ extSel_): before %u %u %u %u of 1000, "
            "during %u %u %u %u of %u\n", (unsigned)lowBefore[0], (unsigned)lowBefore[1], (unsigned)lowBefore[2],
            (unsigned)lowBefore[3], (unsigned)lowDuring[0], (unsigned)lowDuring[1], (unsigned)lowDuring[2],
            (unsigned)lowDuring[3], (unsigned)samples);
        LOG("SIO_FIRST_TEST 9: first low at read (us): halt_ %u (%u) irq_ %u (%u) mpd_ %u (%u) extSel_ %u (%u)\n",
            firstLowOp[0], firstLowUs[0], firstLowOp[1], firstLowUs[1], firstLowOp[2], firstLowUs[2],
            firstLowOp[3], firstLowUs[3]);
        LOG("SIO_FIRST_TEST 9: %s; %u reads off the stack loop in all\n",
            departed ? "left the stack loop" : "stayed in the stack loop", offStack);
        if (!departed) return;
        LOG("SIO_FIRST_TEST 9: no stack loop read in read %u's entries, %u us into the burst; %u writes by "
            "then; its entries from rd[%u] of the last 64 (addr, r/W, raw GPIO_IN)\n", departOp, departUs,
            writes, firstAt);
        for (int k = 0; k < 64; k += 8) {
            LOG("SIO_FIRST_TEST 9: rd[%02d]", k);
            for (int j = k; j < k + 8; j++)
                LOG(" %04x%c:%08x", r0ToAddr(rd[j] >> bmonR0Shift), (rd[j] & bus.rw.mask) ? 'r' : 'W', (unsigned)rd[j]);
            LOG("\n");
        }
        for (unsigned k = 0; k < 16 && k < writes; k++)
            LOG("SIO_FIRST_TEST 9: wr[%u] %04x:%08x\n", k, r0ToAddr(wr[k] >> bmonR0Shift), (unsigned)wr[k]);
    }
};
static BurstWatch burstWatch;   // .bss: 384 bytes of snapshots

static void sioFirstTestBurst(bool stall) {
    constexpr size_t kBytes = 183952;
    constexpr size_t kPiece = 256;
    constexpr uint32_t kBurstCycles = 240u * 400000u;
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
    if (!part || !buf) {
        LOG("SIO_FIRST_TEST %d: no partition or buffer\n", SIO_FIRST_TEST);
        heap_caps_free(buf);
        return;
    }
    core1Probe("before");
    const bool watch = SIO_FIRST_TEST == 9;
    if (watch) burstWatch.begin();
    if (stall) REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
    uint32_t maxCyc = 0, sum = 0;
    size_t done = 0, offset = 0;
    unsigned ops = 0;
    int errs = 0;
    const uint32_t start = XTHAL_GET_CCOUNT();
    while (XTHAL_GET_CCOUNT() - start < kBurstCycles) {
        const uint32_t t = XTHAL_GET_CCOUNT();
        errs += esp_partition_read(part, offset, buf, kPiece) != ESP_OK;
        const uint32_t c = XTHAL_GET_CCOUNT() - t;
        if (c > maxCyc) maxCyc = c;
        sum += buf[0] + buf[kPiece - 1];
        done += kPiece;
        ops++;
        offset = offset + kPiece >= kBytes ? 0 : offset + kPiece;
        if (watch) burstWatch.check(ops, (XTHAL_GET_CCOUNT() - start) / 240);
    }
    const uint32_t total = XTHAL_GET_CCOUNT() - start;
    if (stall) REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
    heap_caps_free(buf);
    LOG("SIO_FIRST_TEST %d: %u bytes in %u reads of %u (core 1 %s) in %u us, slowest %u us, %d errors, sum %u\n",
        SIO_FIRST_TEST, (unsigned)done, ops, (unsigned)kPiece, stall ? "stalled" : "running",
        (unsigned)(total / 240), (unsigned)(maxCyc / 240), errs, (unsigned)sum);
    if (watch) burstWatch.report();
    core1Probe("just after");
    const int64_t until = esp_timer_get_time() + 100000;
    while (esp_timer_get_time() < until) {}
    core1Probe("100 ms after");
}
#endif
#if defined(SIO_TRACE) && SIO_TRACE > 0
// -DSIO_TRACE=<n>: main's first n SIO replies (its own drives, the cold
// path), logged as they return: the DCB after, the result, where the data
// went (COPYBUF for a COPYOUT, else the board's mapped buffer) and its
// first 16 bytes.
static void sioTrace(const PbiIocb *pbiRequest, int result) {
    static int traced = 0;
    if (pbiRequest->cmd != 7 || traced >= SIO_TRACE)
        return;
    traced++;
    const AtariDCB *dcb = atariMem.dcb;
    const uint16_t addr = (uint16_t)(dcb->DBUFHI << 8 | dcb->DBUFLO);
    const uint16_t dbyt = (uint16_t)(dcb->DBYTHI << 8 | dcb->DBYTLO);
    const uint8_t *data = nullptr;
    char where = '-';
    if (result & RES_FLAG_COPYOUT) {
        data = &pbiROM[0x400];
        where = 'C';
    } else if (dbyt > 0 && (data = mmuCheckRangeMapped(mmuStateSaved, addr, dbyt < 16 ? dbyt : 16)) != nullptr) {
        where = 'M';
    }
    LOG("SIO_TRACE %d: D%d cmd %02x aux %02x%02x buf %04x dbyt %u -> DSTATS %02x carry %d y %d res %02x "
        "copybuf %04x copylen %u %c:",
        traced, dcb->DUNIT, dcb->DCOMND, dcb->DAUX2, dcb->DAUX1, addr, dbyt, dcb->DSTATS,
        pbiRequest->carry, pbiRequest->y, result, pbiRequest->copybuf, pbiRequest->copylen, where);
    for (int i = 0; data && i < 16; i++) LOG(" %02x", data[i]);
    LOG("\n");
}
#endif

__attribute__((noinline)) int handlePbiRequestCold(PbiIocb *pbiRequest) {
    const int result = handlePbiRequestColdBody(pbiRequest);
#if defined(SIO_TRACE) && SIO_TRACE > 0
    sioTrace(pbiRequest, result);
#endif
    return result;
}

static int handlePbiRequestColdBody(PbiIocb *pbiRequest) {
    //if (config.haltAvailable == false) pinReleaseMask |= bus.halt_.mask;
    //busyWait6502Ticks(5000);  // why does this cause immediate 13.8sec stackprog timeouts?
    if (pbiRequest->cmd == PBICMD_PROBE_BEGIN) {   // before the screen mapping below:
                                                   // SAVMSC isn't set up at PBI init
        if (!config.autoMem)
            return RES_FLAG_COMPLETE | RES_FLAG_PROBE_SKIP;   // the overlay returns at once
        memProbeBegin();
        return RES_FLAG_COMPLETE;
    }
    if (pbiRequest->cmd == PBICMD_PROBE_REPORT) {
        memProbeReport();
        return RES_FLAG_COMPLETE;
    }
 
    // only do this once, don't try and re-map and follow screen mem around if it moves
    DRAM_ATTR static bool screenMemMapped = false;
    if (!screenMemMapped) { 
        int savmsc = (atariRam[89] << 8) + atariRam[88];
        int len = 20 * 40;
        if (mmuCheckRangeMapped(mmuStateSaved, savmsc, len) == NULL) {
            int numPages = pageNr(savmsc + len) - pageNr(savmsc) + 1;
            if (screenMem == NULL) { 
                // TODO: Consider PSRAM for this Atari screen-memory copy buffer to save internal SRAM.
                screenMem = (uint8_t *)heap_caps_malloc(numPages * pageSize, MALLOC_CAP_INTERNAL);
                assert(screenMem != NULL);
            }
            if(!pbiCopyAndMapPagesIntoBasemem(pbiRequest, pageNr(savmsc), numPages, screenMem))
                return RES_FLAG_NEED_COPYIN;
        }
        screenMemMapped = true;
        atariCart.initMmuBlocks(); // TODO HACK copy the changed ram mappings into the cart banks
    }

    if (pbiRequest->cmd == PBICMD_UNMAP_NATIVE_BLOCK) { 
        mmuUnmapRange(NATIVE_BLOCK_ADDR, NATIVE_BLOCK_ADDR + NATIVE_BLOCK_LEN - 1);
        //waitVblank(3700000);
        return RES_FLAG_COMPLETE;
    } else if (pbiRequest->cmd == PBICMD_REMAP_NATIVE_BLOCK) { 
        mmuRemapBaseRam(NATIVE_BLOCK_ADDR, NATIVE_BLOCK_ADDR + NATIVE_BLOCK_LEN - 1);
        //waitVblank(3700000);
        return RES_FLAG_COMPLETE;
    } else if (pbiRequest->cmd == PBICMD_WAIT_VBLANK) { // wait for good vblank timing
        waitVblank(0.0 * 1000000);
        return RES_FLAG_COMPLETE;
    } else if (pbiRequest->cmd == PBICMD_NOP) {
        mmuUnmapRange(NATIVE_BLOCK_ADDR, NATIVE_BLOCK_ADDR + NATIVE_BLOCK_LEN - 1);
        return RES_FLAG_COMPLETE;
    }

    //ScopedPrintStructLog ps(pbiRequest);
    if (structLogs) structLogs->pbi.add(*pbiRequest);
    handleSerialIO(pbiRequest);

    uint8_t iocbNumber = pbiRequest->x / sizeof(AtariIOCB);
    AtariIOCB *iocb = atariMem.ziocb;
    //pbiRequest->y = 1; // assume success
    //pbiRequest->carry = 0; // assume fail 
    if (pbiRequest->cmd == 1) { // open
        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 0; // assume fail 
        bool openSucceeded = cioFiles->open(pbiRequest, iocb, iocbNumber);
        if ((pbiRequest->result & RES_FLAG_NEED_COPYIN) != 0) {
            return RES_FLAG_NEED_COPYIN;
        }
        if (openSucceeded) {
            pbiRequest->carry = 1;
        } else {
            pbiRequest->carry = 0;
        }
        if (config.fakeCio) {
            ioCount++;
            lastIoSec = elapsedSec;
        }
    } else if (pbiRequest->cmd == 2) { // close
        pbiRequest->y = 1; 
        cioFiles->close(pbiRequest, iocbNumber);
        if ((pbiRequest->result & RES_FLAG_NEED_COPYIN) != 0)
            return RES_FLAG_NEED_COPYIN;
        pbiRequest->carry = 1; 
        if (config.fakeCio) {
            ioCount++;
            lastIoSec = elapsedSec;
        }
    } else if (pbiRequest->cmd == 3) { // get
        pbiRequest->y = 1; 
        int c = cioFiles->get(pbiRequest, iocbNumber);
        if ((pbiRequest->result & RES_FLAG_NEED_COPYIN) != 0)
            return RES_FLAG_NEED_COPYIN;
        if (c < 0) 
            pbiRequest->y = 136;
        else
            pbiRequest->a = c; 
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 4) { // put
        pbiRequest->y = 1; 
        if (!cioFiles->put(pbiRequest, iocbNumber, pbiRequest->a))
            pbiRequest->y = 136;
        if ((pbiRequest->result & RES_FLAG_NEED_COPYIN) != 0)
            return RES_FLAG_NEED_COPYIN;
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 5) { // status 
        pbiRequest->y = 1; 
        pbiRequest->carry = 0; // assume fail 
    } else if (pbiRequest->cmd == 6) { // special 
        pbiRequest->y = 1; 
        pbiRequest->carry = 0; // assume fail 
    } else if (pbiRequest->cmd == 7) { // low level io, see DCB
        SCOPED_BLINK_LED(20,0,0);
        pbiRequest->y = 1; 
        pbiRequest->carry = 0; // assume fail 
        AtariDCB *dcb = atariMem.dcb;
        uint16_t addr = (((uint16_t)dcb->DBUFHI) << 8) | dcb->DBUFLO;
        int sector = (((uint16_t)dcb->DAUX2) << 8) | dcb->DAUX1;
        if (structLogs) structLogs->dcb.add(*dcb);
        if (0) { 
            LOG(DRAM_STR("DCB: "));
            StructLog<AtariDCB>::printEntry(*dcb);
            fflush(stdout);
        }
#ifdef CSIM
        const int ideResult = handlePbiAptDcb(pbiRequest, dcb);
        // TODO: handle immediate failout with NEED_COPYIN
        if (ideResult >= 0) {
            lastIoSec = elapsedSec;
            ioCount++;
            return ideResult;
        }
#endif
        if (dcb->DDEVIC == 0x31 && dcb->DUNIT >= 1 && dcb->DUNIT < 9) {  // Device D1:
            DiskImage *disk = sio.use(dcb->DUNIT - 1);   // opens a drive mounted on use (cold)
#if defined(SIO_FIRST_STALL_MS) && SIO_FIRST_STALL_MS > 0
            // -DSIO_FIRST_STALL_MS=<ms>: a busy wait on D1's first command, as
            // long as a first-use gzip open (~142 ms) but no file I/O, no
            // allocation (a test: is the detach's length the trigger on .5?).
            DRAM_ATTR static bool stalled = false;
            if (!stalled && dcb->DUNIT == 1) {
                stalled = true;
                const int64_t until = esp_timer_get_time() + SIO_FIRST_STALL_MS * 1000LL;
                while (esp_timer_get_time() < until) {}
                LOG("SIO_FIRST_STALL_MS: D1's first command stalled %d ms\n", SIO_FIRST_STALL_MS);
            }
#endif
#if defined(SIO_FIRST_TEST) && SIO_FIRST_TEST > 0
            // -DSIO_FIRST_TEST=<n>: one piece of a first-use open, on D1's first
            // command (a test on .5, with -DSIO_MOUNT_ON_USE=0):
            //   1 a 184 KB PSRAM malloc and free, untouched
            //   2 the same, memset first (the PSRAM fill)
            //   3 184 KB read from SPIFFS (SIO_FIRST_TEST_FILE, rewound as
            //     need be) into a 4 KB internal buffer (the flash burst)
            //   4 as 3, then 150 ms idle (does something linger after it?)
            //   5 as 3 with core 1 stalled (RUNSTALL) through it: it's on the
            //     transparent map, driving nothing (as the exit path stalls it)
            //   6 184 KB of the spiffs partition read through the cache
            //     (esp_partition_mmap): no SPI1 flash operations, no cache
            //     suspends
            //   7 400 ms of 256-byte esp_partition_read()s of the spiffs
            //     partition (SPI1 operations, no SPIFFS), core 1 running, with core 1
            //     probed before, just after and 100 ms after (core1Probe());
            //     run with -DCOLD_CORE1_STALL=0 -DBMON_DELAY_LOG=1 (core 0's
            //     backlog after the re-attach, in the io: lines)
            //   8 as 7, core 1 stalled through the burst
            //   9 as 7, watched (BurstWatch): the first read off the stack
            //     loop and the ring around it, and halt_/irq_/mpd_/extSel_
            DRAM_ATTR static bool tested = false;
            if (!tested && dcb->DUNIT == 1) {
                tested = true;
                constexpr size_t kBytes = 183952;
                const int64_t start = esp_timer_get_time();
                size_t done = 0;
                if (SIO_FIRST_TEST >= 7) {
#if SIO_FIRST_TEST >= 7 && !defined(CSIM)
                    sioFirstTestBurst(SIO_FIRST_TEST == 8);
#endif
                    done = kBytes;
                } else if (SIO_FIRST_TEST == 6) {
                    const esp_partition_t *part =
                        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
                    const void *mapped = nullptr;
                    esp_partition_mmap_handle_t handle;
                    const size_t len = (kBytes + 0xffff) & ~0xffffu;
                    if (part && esp_partition_mmap(part, 0, len, ESP_PARTITION_MMAP_DATA, &mapped, &handle) == ESP_OK) {
                        uint32_t sum = 0;
                        for (size_t i = 0; i < kBytes; i += 4) sum += *(const volatile uint32_t *)((const uint8_t *)mapped + i);
                        esp_partition_munmap(handle);
                        done = kBytes;
                        LOG("SIO_FIRST_TEST 6: sum %08x\n", (unsigned)sum);
                    }
                } else if (SIO_FIRST_TEST <= 2) {
                    if (uint8_t *p = (uint8_t *)heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM)) {
                        if (SIO_FIRST_TEST == 2) memset(p, 0x5a, kBytes);
                        heap_caps_free(p);
                        done = kBytes;
                    }
                } else {
#ifndef SIO_FIRST_TEST_FILE
#define SIO_FIRST_TEST_FILE "/spiffs/d1.atr"
#endif
                    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
                    if (SIO_FIRST_TEST == 5)
                        REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
                    FILE *f = fopen(SIO_FIRST_TEST_FILE, "rb");
                    while (buf && f && done < kBytes) {
                        const size_t n = fread(buf, 1, std::min<size_t>(4096, kBytes - done), f);
                        if (n == 0) {
                            if (done == 0) break;   // empty file
                            rewind(f);
                            continue;
                        }
                        done += n;
                    }
                    if (f) fclose(f);
                    if (SIO_FIRST_TEST == 5 && !COLD_CORE1_STALL)
                        REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
                    heap_caps_free(buf);
                    if (SIO_FIRST_TEST == 4) {
                        const int64_t until = esp_timer_get_time() + 150000;
                        while (esp_timer_get_time() < until) {}
                    }
                }
                LOG("SIO_FIRST_TEST %d: %u of %u bytes in %d ms\n", SIO_FIRST_TEST, (unsigned)done,
                    (unsigned)kBytes, (int)((esp_timer_get_time() - start) / 1000));
            }
#endif
            lastIoSec = elapsedSec;
            ioCount++;
            if (disk == NULL || disk->valid() == false) { 
                pbiRequest->carry = 0;
                dcb->DSTATS = 0x8A;
                return RES_FLAG_COMPLETE;
            }
            int sectorSize = disk->sectorSize();
            int dbyt = (dcb->DBYTHI << 8) + dcb->DBYTLO;
            // The data goes through sioStage, scattered to or gathered from
            // the 6502's buffer wherever and however much of it the board
            // maps (pbiScatter()/pbiGather(), pbi.h): direct for the mapped
            // runs, COPYBUF for the rest.
            static DRAM_ATTR uint8_t sioStage[512];
            if (dbyt > (int)sizeof(sioStage)) {
                dcb->DSTATS = 0;   // bigger than any sector
                pbiRequest->carry = 0;
                return RES_FLAG_COMPLETE;
            }
            bool allCopied = false;
#ifdef CSIM
            // for csim code coverage, send half the IOs whole through COPYBUF
            static bool alternate = false;
            allCopied = alternate;
            alternate = !alternate;
#endif

            if (dcb->DCOMND == 0x53) { // SIO status command
                // drive status https://www.atarimax.com/jindroush.atari.org/asio.html
                sioStage[0] = (sectorSize != 128) ? 0x20 : 0x00; // bit 0 = frame err, 1 = cksum err, wr err, wr prot, motor on, sect size, unused, med density  
                sioStage[1] = 0xff; // inverted bits: busy, DRQ, data lost, crc err, record not found, head loaded, write pro, not ready 
                sioStage[2] = 0xff; // timeout for format 
                sioStage[3] = 0xff; // copy of wd
                dcb->DSTATS = 0x1;
                pbiRequest->carry = 1;
                return pbiScatter(mmuStateSaved, pbiRequest, addr, 4, sioStage, allCopied);
            }
            
            // The common case: the whole sector is one contiguous mapped run
            // (and no copy-in is pending, which would win): the disk image
            // reads or writes the 6502's buffer in place, no sioStage.
            const int bytes = disk->specificSectorSize(sector);
            uint8_t *direct = nullptr;
            if (!allCopied && (pbiRequest->req & REQ_FLAG_COPYIN) == 0 && dbyt == bytes && dbyt > 0)
                direct = mmuCheckRangeMapped(mmuStateSaved, addr, dbyt);

            if (dcb->DCOMND == 0x52 || dcb->DCOMND == 0xd2) {  // READ sector
                if (direct != nullptr) {
                    if (disk->read(direct, sector) != (size_t)bytes) {
                        dcb->DSTATS = 0;
                        pbiRequest->carry = 0;
                        return RES_FLAG_COMPLETE;
                    }
                    dcb->DSTATS = 0x1;
                    pbiRequest->carry = 1;
                    return RES_FLAG_COMPLETE;
                }
                if (bytes > (int)sizeof(sioStage) || disk->read(sioStage, sector) != (size_t)bytes) {
                    dcb->DSTATS = 0;   // as a failed write: no garbage sector
                    pbiRequest->carry = 0;
                    return RES_FLAG_COMPLETE;
                }
                dcb->DSTATS = 0x1;
                pbiRequest->carry = 1;
                return pbiScatter(mmuStateSaved, pbiRequest, addr, dbyt, sioStage, allCopied);
            }
            if (dcb->DCOMND == 0x50 || dcb->DCOMND == 0xd0 || dcb->DCOMND == 0x57) {  // WRITE sector (ignore verify)
                uint8_t *from = direct;
                if (from == nullptr) {
                    // A short DBYT writes zeros past it, not the last I/O's bytes.
                    memset(sioStage + dbyt, 0, sizeof(sioStage) - dbyt);
                    if (!pbiGather(mmuStateSaved, pbiRequest, addr, dbyt, sioStage, allCopied))
                        return RES_FLAG_NEED_COPYIN;
                    from = sioStage;
                }
                int writeResult = disk->write(from, sector);
                if (writeResult < 0) {
                    dcb->DSTATS = 0;
                    pbiRequest->carry = 0;
                    return RES_FLAG_COMPLETE;
                }
                dcb->DSTATS = 0x1;
                pbiRequest->carry = 1;
                return RES_FLAG_COMPLETE;
            }
            if (dcb->DCOMND == 0x3f) {  // get hi-speed capabilities
                dcb->DSTATS = 0x1;
                pbiRequest->carry = 1;
                sioStage[0] = 0x28;
                return pbiScatter(mmuStateSaved, pbiRequest, addr, 1, sioStage, allCopied);
            }
            if (dcb->DCOMND == 0x48) {  // HAPPY command
                dcb->DSTATS = 0x1;
                pbiRequest->carry = 1;
            }
            if (dcb->DCOMND == 0x4e) {  // read percom block
                struct PercomBlock {
                    uint8_t tracks;
                    uint8_t stepRate;
                    uint8_t secPerTrkHi;
                    uint8_t secPerTrkLo;
                    uint8_t sides;
                    uint8_t mfm;
                    uint8_t bytesPerSectorHi;
                    uint8_t bytesPerSectorLo;
                    uint8_t driveOnline;
                    uint8_t unused[3];
                };
                PercomBlock *percom = (PercomBlock *)sioStage;
                int sectors = disk->sectorCount();
                percom->tracks = 1;
                percom->stepRate = 3;
                percom->secPerTrkHi = sectors >> 8;
                percom->secPerTrkLo = sectors & 0xff;
                percom->sides = 1;
                percom->mfm = 4;
                percom->bytesPerSectorHi = sectorSize >> 8;
                percom->bytesPerSectorLo = sectorSize & 0xff;
                percom->driveOnline = 0xff;
                percom->unused[0] = percom->unused[1] = percom->unused[2] = 0;
                dcb->DSTATS = 0x1;
                pbiRequest->carry = 1;  
                return pbiScatter(mmuStateSaved, pbiRequest, addr,
                                  dbyt < (int)sizeof(PercomBlock) ? dbyt : (int)sizeof(PercomBlock), sioStage,
                                  allCopied);
            }
            if (dcb->DCOMND == 0x4f) {  // write percom block
                if (!pbiGather(mmuStateSaved, pbiRequest, addr, dbyt, sioStage, allCopied))
                    return RES_FLAG_NEED_COPYIN;
                dcb->DSTATS = 0x1;
                pbiRequest->carry = 1;
            }
        }
    } else if (pbiRequest->cmd == 8) { // IRQ
        clearInterrupt();
        pbiInterruptCount++;
        SCOPED_BLINK_LED(0,0,20);
        if (wifiInitialized || (config.wifiConnectDelay > 0 && elapsedSec > config.wifiConnectDelay))
            wifiRun();
        telnetScreenRun();   // the Sidecar's telnet client (a no-op without the link)
        if (screenshotRequested) {   // the Sidecar's SetFlags Screenshot
            screenshotRequested = 0;
            screenshotToSidecar();
        }
        if (rebootRequested)   // requestSafeReboot(): never returns
            safeRebootParked();


#if CONFIG_A8BB_SYSMONITOR
    } else if (pbiRequest->cmd == 11) { // system monitor
        SCOPED_BLINK_LED(0,20,0);
        sysMonitorRequested = 0;
        sysMonitor->pbi(pbiRequest);
        sysMonitor->runColdWork();   // e.g. a cartridge change: we're cold here
#endif
    } else  if (pbiRequest->cmd == 20) {
#if 0 
        uint32_t stsc = XTHAL_GET_CCOUNT();
        for(int i = 0; i < 4 * 1024; i++) 
            psram[i] = *((uint32_t *)&atariRam[i * 4]);
        for(int i = 0; i < 4 * 1024; i++) 
            *((uint32_t *)&atariRam[i * 4]) = psram[i];
        int elapsed = XTHAL_GET_CCOUNT() - stsc;
        LOG("%d ticks to copy 16KB twice\n", elapsed);
        yield();
#endif
    } else if (pbiRequest->cmd == PBICMD_SET_MONITOR_BOOT) {
        config.cartImage = "/SDX450_maxflash1.car.gz";
        config.save();
        openCartImage(config.cartImage);
        mmuOnChange(/*force==*/true);
    }
    return RES_FLAG_COMPLETE;
}

DRAM_ATTR int enableBusInTicks = 0;
PbiIocb *lastPbiReq;
DRAM_ATTR int requestLeaveHalted = 0;
// A safe reboot (main.cpp's requestSafeReboot()): the next IRQ call
// reboots main from the cold handler, the 6502 parked in its own RAM.
DRAM_ATTR volatile int rebootRequested = 0;

#ifdef ATARI800_SIM
extern "C" CORE0_HOTSPOTS_ATTR void handlePbiRequestHot(PbiIocb *pbiRequest) {
#else
CORE0_HOTSPOTS_ATTR void handlePbiRequestHot(PbiIocb *pbiRequest) {
#endif
    CORE0_PHASE(PH_PBI, 0);

    pbiRequest->result = 0;
    if (diskRotatePending) {   // the idle disk swap (hostSio.h)
        diskRotatePending = false;
        rotateDrives();
    }
#if A8BB_SIDECAR
#if A8BB_SIDECAR_LOOPBACK
    sidecarHost.probe(5000); // simulate overhead of offloaded pbi call
#else
    // Requests for the Sidecar's drives go to sidecarHost instead of
    // handlePbiRequestCold() (below).
    if (sidecarHost.checkPbiRequestHook(pbiRequest)) {
        pbiRequest->result |= sidecarHost.handlePbiRequest(pbiRequest);
        // PROFILE_MODE=GPIO: sample the live bus right after the Sidecar
        // call, still before the 6502 is released (req is still set).
        PROFILE_BUS();
        pbiRequest->req = 0;
        return;
    }
#endif
#endif

    // Investigating halting the cpu instead of the stack-prog wait scheme
    // so far doens't work.
    
    // Assume pbi commands are always issued with the 6502 ready for a bus detach
    //pbiRequest->req = 0x2;

    //if (needSafeWait(pbiRequest))
    //    return;
//#define HALT_6502
#ifdef HALT_6502
    halt6502();
    //resume6502();
#endif
    // PROFILE_MODE=GPIO: sample the live bus before ScopedInterruptEnable
    // detaches it; the 6502 is in the stack-resident wait loop, so the
    // sample sees board and native cycles both.
    PROFILE_BUS();
    if (!needSafeWait(pbiRequest)) {   // else it asks for a detach-safe reissue
        // Detach, run the cold part from flash, re-attach (the destructor),
        // all from here: the code on either side of the detach is IRAM.
        ScopedInterruptEnable intEn;
        pbiRequest->result |= handlePbiRequestCold(pbiRequest);
    }

#if CONFIG_A8BB_SYSMONITOR
    if (pbiRequest->consol == 1 || pbiRequest->kbcode == 0xe5 || sysMonitorRequested)  {
        pbiRequest->result |= RES_FLAG_MONITOR;
    }
#endif
    bmonTail = bmonHead;
#ifdef HALT_6502
    busyWait6502Ticks(100);
    resume6502();
    busyWait6502Ticks(5);
#endif
    bmonTail = bmonHead;
    if ((pbiRequest->req & REQ_FLAG_STACKWAIT) != 0) {
        // The 6502 is in the stack program (SAFE_WAIT, pbirom.asm): the
        // request was its store at $0100. It spins on a BCS (D5-proof bytes)
        // until we write an RTS over it.
        constexpr uint16_t stackLoop =
            0x100 + (PBIROM_stack_res_loop - PBIROM_stack_res_wait);
#if !defined(CSIM) || defined(CSIM_BUS_CYCLE)   // csim's default mode: no bus cycles
        // First see it there: fetches of the BCS in the read ring (which
        // has no refresh cycles), counted from now, so nothing from before
        // or during the detach counts. None in 1 s: the 6502 is wedged
        // somewhere (a corrupted fetch): -3. The loop writes nothing.
        static constexpr DRAM_ATTR uint32_t sprogTimeout = 240000000;
        static constexpr int kStackLoopReads = 4;
        const uint32_t startTsc = XTHAL_GET_CCOUNT();
        int seen = 0;
        bmonRdTail = bmonRdHead;
        while (seen < kStackLoopReads) {
            CSIM_HOOK();
            bmonTail = bmonHead;   // writes: none expected, none matter
            // Warning: this is a core0 ifetch hotspot, with a hand-tuned loop delay.
#pragma GCC unroll 8
            for (int poll = 0; poll < 8 && bmonRdHead == bmonRdTail; ++poll) {
                loopNezDelay(33);   // 30 passes + setup
            }
            const unsigned int head = bmonRdHead;
            if (head - bmonRdTail > bmonRdArraySz) bmonRdTail = head - bmonRdArraySz;   // lapped
            for (; bmonRdTail != head && seen < kStackLoopReads; bmonRdTail++) {
                const uint32_t r0 = bmonRdArray[bmonRdTail & bmonRdArraySzMask].r0 >> bmonR0Shift;
                seen += r0ToAddr(r0) == stackLoop;
            }
            if (XTHAL_GET_CCOUNT() - startTsc > sprogTimeout) {
                CORE0_EXIT_HOT(1, "-3 stackprog timeout");
                return; // main loop will exit
            }
        }
        bmonTail = bmonHead;
#endif
        pbiRequest->req = 0;
        bmonRdTail = bmonRdHead;   // the request's reads (the driver, the stack loop): none matter
        atariRam[stackLoop] = 0x60;   // RTS over the BCS
    } else { 
        bmonTail = bmonHead;
        bmonRdTail = bmonRdHead;
        pbiRequest->req = 0;
    }
#ifdef HALT_6502
//    busyWait6502Ticks(100);
//    resume6502();
//    busyWait6502Ticks(5);
#endif
}
