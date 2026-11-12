#ifdef CSIM
#include "csim.h"
#endif
#include "asmDefs.h"
#define PBIROM_SYMBOLS_ONLY
#include "pbirom.h"   // PBIROM_<label> addresses
#include "screenScrape.h"

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
#include "telnet.h"
#include "arduinoLite.h"
#if CONFIG_A8BB_SIDECAR_ENABLE
#include "sidecarHost.h"
#endif

using std::min;
using std::max;

void sendHttpRequest();
void connectWifi();
void connectToServer();
void start_webserver(void);
#if CONFIG_A8BB_ENABLE_MDNS
void startMdnsWithMacHostname();
#endif

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

struct ScopedInterruptEnable { 
    uint32_t oldint;
    inline ScopedInterruptEnable() { 
        unmapCount++;
        disableBus();
        busyWait6502Ticks(20);
        enableCore0WDT();
        portENABLE_INTERRUPTS();
        yield();
    }
    inline ~ScopedInterruptEnable() {
        yield();
        portDISABLE_INTERRUPTS();
        //ASM("rsil %0, 15" : "=r"(oldint) : : );
        disableCore0WDT();
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
//#define SCOPED_INTERRUPT_ENABLE(pbiReq) if (needSafeWait(pbiReq)) return; ScopedInterruptEnable intEn;  
#define SCOPED_INTERRUPT_ENABLE(pbiReq) ScopedInterruptEnable intEn;  


volatile bool wifiInitialized = false;
IRAM_ATTR void wifiRun() { 
    if (config.enableWifi == false)
        return;

    if (wifiInitialized == false) { 
        connectWifi(); // 82876 bytes 
        start_webserver();  //12516 bytes 
        //smbReq();
        startTelnetServer();
#if CONFIG_A8BB_ENABLE_MDNS
        if (config.enableMdns)
            startMdnsWithMacHostname();
#endif
        wifiInitialized = true;
        for(int n = 0; n < Sio::kDrives; n++) {
            // TMP disable until better error handling 
            //if (sio.disk(n) != NULL) sio.disk(n)->start();
        }
    }
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

void IRAM_ATTR waitVblank(int offset) { 
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

void IFLASH_ATTR handleSerialInput() {
    uint8_t c;
    while(usb_serial_jtag_read_bytes((void *)&c, 1, 0) > 0) { 
        static DRAM_ATTR LineBuffer lb;
        lb.add(c, [](const char *line) {
            char x;
            if (sscanf(line, DRAM_STR("key %c"), &x) == 1) {
                putKey(x);
            } else if (sscanf(line, DRAM_STR("exit %c"), &x) == 1) {
                core0_exit(1, "User-requested halt '%c'", x);
            } else if (sscanf(line, DRAM_STR("screen %c"), &x) == 1) {
                dumpScreenToSerial(x);
            }
        });
    }
}

void IFLASH_ATTR dumpScreenToSerial(char tag, uint8_t *mem/*= NULL*/) {
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    if (mem == NULL) {
        mem = mmuCheckRangeMapped(mmuStateSaved, savmsc, 24 * 40);
        if (mem == NULL) {
            printf(DRAM_STR("SCREEN%c 00 memory at SAVMSC(%04x) not mapped, no screendump\n"), tag, savmsc);
            return;
        }
    }

    printf(DRAM_STR("SCREEN%c 00 memory at SAVMSC(%04x):\n"), tag, savmsc);
    printf(DRAM_STR("SCREEN%c 01 +----------------------------------------+\n"), tag);
    for(int row = 0; row < 24; row++) { 
        printf(DRAM_STR("SCREEN%c %02d |"), tag, row + 2);
        for(int col = 0; col < 40; col++) { 
            uint8_t c = *(mem + row * 40 + col);
            char buf[16];
            screenMemToAscii(buf, sizeof(buf), c);
            printf("%s", buf);
        }
        printf(DRAM_STR("|\n"));
    }
    printf(DRAM_STR("SCREEN%c 27 +----------------------------------------+\n"), tag);
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

// Called from a pbiRequest context to copy in memory from 6502 native ram. 
//   Sets REQ_FLAG_COPYIN and returns false until successive pbi requests 
//   have completed the transfer, then finally returns true
bool IRAM_ATTR pbiReqCopyIn(PbiIocb *pbiRequest, uint16_t start, uint16_t len, uint8_t *mem) {    
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

bool IRAM_ATTR pbiCopyAndMapPages(PbiIocb *p, int startPage, int pages, uint8_t *mem) {
    if (!pbiReqCopyIn(p, startPage * pageSize, pages * pageSize, mem))
        return false;
    mmuMapRangeRW(startPage * pageSize, (startPage + pages) * pageSize - 1, mem);
    return true;
}

// called from a pbi command context to copy the data currently in native 6502 pages 
// into esp32 memory and map them.  
//   Sets REQ_FLAG_COPYIN and returns false until successive pbi requests 
//   have completed the transfer, then finally returns true

bool IRAM_ATTR pbiCopyAndMapPagesIntoBasemem(PbiIocb *p, int startPage, int pages, uint8_t *mem) {
    if (!pbiCopyAndMapPages(p, startPage, pages, mem))
        return false;
    mmuAddBaseRam(startPage * pageSize, (startPage + pages) * pageSize - 1, mem);
    return true;           
}

IRAM_ATTR void handleSerialIO(PbiIocb *pbiRequest) { 
    if (0 && (pbiRequest->result & (RES_FLAG_NEED_COPYIN | RES_FLAG_COPYOUT)) != 0) { 
        printf("copy in/out result=0x%02x, addr 0x%04x len %d\n", 
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
    DRAM_ATTR static int lastPrint = -999;
    if (elapsedSec - lastPrint >= config.serialStatusSec) {
        handleSerialInput();
        lastPrint = elapsedSec;
        DRAM_ATTR static int lastIoCount = 0;
        printf(DRAM_STR("%02d:%02d:%02d io: %8d (%3d) irq %d "
            "halt %d evict %d/%d"),
            elapsedSec/3600, (elapsedSec/60)%60, elapsedSec%60, ioCount,  
            ioCount - lastIoCount, 
            pbiInterruptCount, haltCount, extMem.evictCount, extMem.swapCount);
#if CONFIG_A8BB_SIDECAR_ENABLE
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
            printf(DRAM_STR(" sidecar new %u (%u) err %u (%u)"),
                   (unsigned)link.reads_new, (unsigned)(link.reads_new - lastLink.reads_new),
                   (unsigned)faults(link), (unsigned)newFaults);
            if (newFaults) {
                printf(DRAM_STR(" [torn %u crc %u nolock %u stalled %u]"),
                       (unsigned)(link.reads_torn - lastLink.reads_torn),
                       (unsigned)(link.reads_crc - lastLink.reads_crc),
                       (unsigned)(link.reads_no_lock - lastLink.reads_no_lock),
                       (unsigned)(link.reads_stalled - lastLink.reads_stalled));
            }
            lastLink = link;
        }
#endif
        printf(DRAM_STR("\n"));
        fflush(stdout);
        if (BUS_ANALYZER && elapsedSec > 12) { 
            printf("DONE\n");
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

// The halt-line test (docs/memory-autodetect.md): does this machine's halt
// line stop the 6502? An XL's PBI has the line; a 130XE's ECI doesn't, and
// that also tells an upgraded 800XL (native banks, halt) from a 130XE.
// Run by handlePbiRequest() for PBICMD_PROBE_BEGIN, after the stack-program
// wait: the 6502 is in the stack-resident loop, whose INC DUMMY_WRITE
// changes that byte every pass (~16 cycles), the bus is attached and core
// 0's interrupts are masked. Page 1's read ctrl gets the halt_ bit and
// pinReleaseMask drops it, so core 1 asserts and holds halt_ on the loop's
// next instruction fetch. Three samples of DUMMY_WRITE ~100 6502 cycles
// apart: all equal means the 6502 stopped. Then the bit is cleared and the
// 6502 released. With the line it's held ~300 cycles.
DRAM_ATTR int haltLineDetected = -1;   // -1 untested, 0 no, 1 yes
static DRAM_ATTR uint8_t haltTestSamples[3];

static IRAM_ATTR void haltTestSetStackPageHalt(bool on) {
    for (int vid : PAGESEL_EXTRA_VARIATIONS) {
        auto &ctrl = l1Blocks[0].ctrl[1 | PAGESEL_RD | vid];   // page 1, reads
        ctrl = on ? (ctrl | bus.halt_.mask) : (ctrl & bus.halt_.maskInverse);
    }
}

static IRAM_ATTR void haltLineTest() {
    volatile uint8_t *const dummy = &pbiROM[PBIROM_DUMMY_WRITE - 0xd800];
    const uint32_t savedReleaseMask = mmuState.pinReleaseMask;
    mmuState.pinReleaseMask &= bus.halt_.maskInverse;   // hold it once asserted
    haltTestSetStackPageHalt(true);                       // assert on the next fetch
    busyWait6502Ticks(50);                                // let it take hold
    for (uint8_t &sample : haltTestSamples) {
        sample = *dummy;
        busyWait6502Ticks(100);
    }
    haltTestSetStackPageHalt(false);
    mmuState.pinReleaseMask |= bus.halt_.mask;           // release, as resume6502()
    busyWait6502Ticks(15);
    mmuState.pinReleaseMask = savedReleaseMask;
    haltLineDetected = (haltTestSamples[0] == haltTestSamples[1]
                        && haltTestSamples[1] == haltTestSamples[2]) ? 1 : 0;
}

// The native memory probe (docs/memory-autodetect.md), a sketch: the
// overlay at $DC00 (pbirom.asm) brackets its probe with these two
// commands. Both run with the bus detached (inside SCOPED_INTERRUPT_ENABLE),
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
        printf("memory probe: no RAM for the saved map; not probing\n");
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

static void memProbeReport() {
    const uint8_t *ram = (const uint8_t *)&pbiROM[PBIROM_PROBE_RAM - 0xd800];
    const uint8_t *banks = (const uint8_t *)&pbiROM[PBIROM_PROBE_BANKS - 0xd800];
    if (!memProbeActive) {
        printf("memory probe: report without a probe map; ignored\n");
        return;
    }
    // Native RAM: the 1 KB blocks above $4000 that held both patterns.
    int topKb = 16;
    while (topKb < 48 && ram[topKb - 16]) topKb++;
    char map[33];
    for (int i = 0; i < 32; i++) map[i] = ram[i] ? 'R' : '.';
    map[32] = 0;
    printf("memory probe: halt line %s (DUMMY_WRITE %02x %02x %02x)\n",
           haltLineDetected == 1 ? "present (the 6502 stopped)" :
           haltLineDetected == 0 ? "absent (the 6502 kept running)" : "untested",
           haltTestSamples[0], haltTestSamples[1], haltTestSamples[2]);
    printf("memory probe: native RAM to %dK; $4000-$BFFF by 1K: %s\n", topKb, map);
    // Banks: PORTB (v << 2) | 3 for v = 0..63, signature v ^ $A5, written
    // from 63 down to 0. A value that kept its own signature has RAM of its
    // own; the others show the last signature written to the same RAM.
    // Only meaningful with RAM at $4000.
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
        printf("memory probe: %d distinct native banks\n", memProbeNativeBanks);
        int own = 0;
        for (int v = 0; v < 64; v++) own += banks[v] == (v ^ 0xa5);
        printf("memory probe: %d PORTB values with their own RAM at $4000; groups (PORTB values sharing it):\n", own);
        bool shown[64] = {};
        for (int v = 0; v < 64; v++) {
            if (shown[v]) continue;
            printf("  $%02x:", banks[v]);
            for (int w = v; w < 64; w++)
                if (!shown[w] && banks[w] == banks[v]) { printf(" %02x", (w << 2) | 3); shown[w] = true; }
            printf("\n");
        }
    }
    mmuStateSaved.safeCopy(*memProbeSaved);
    heap_caps_free(memProbeSaved);
    memProbeSaved = nullptr;
    memProbeActive = false;
}

IRAM_ATTR __attribute__((noinline)) int handlePbiRequest2(PbiIocb *pbiRequest) {
    //if (config.haltAvailable == false) pinReleaseMask |= bus.halt_.mask;
    //busyWait6502Ticks(5000);  // why does this cause immediate 13.8sec stackprog timeouts?
    SCOPED_INTERRUPT_ENABLE(pbiRequest);
    if (pbiRequest->cmd == PBICMD_PROBE_BEGIN) {   // before the screen mapping below:
                                                   // SAVMSC isn't set up at PBI init
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
            printf(DRAM_STR("DCB: "));
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
            DiskImage *disk = sio.disk(dcb->DUNIT - 1); 
            lastIoSec = elapsedSec;
            ioCount++;
            if (disk == NULL || disk->valid() == false) { 
                pbiRequest->carry = 0;
                return RES_FLAG_COMPLETE;
            }
            int sectorSize = disk->sectorSize();
            int dbyt = (dcb->DBYTHI << 8) + dcb->DBYTLO;
            pbiRequest->copylen = dbyt;
            pbiRequest->copybuf = addr;

            uint8_t *paddr = mmuCheckRangeMapped(mmuStateSaved, addr, dbyt);
#ifdef CSIM
            static bool alternate = false;
            if (alternate) paddr = NULL;
            alternate = !alternate;
#endif
            bool copyRequired = (paddr == NULL);
            if (copyRequired) {  
                paddr = &pbiROM[0x400];
            }

            if (dcb->DCOMND == 0x53) { // SIO status command
                pbiRequest->copylen = 4;
                // drive status https://www.atarimax.com/jindroush.atari.org/asio.html
                paddr[0] = (sectorSize != 128) ? 0x20 : 0x00; // bit 0 = frame err, 1 = cksum err, wr err, wr prot, motor on, sect size, unused, med density  
                paddr[1] = 0xff; // inverted bits: busy, DRQ, data lost, crc err, record not found, head loaded, write pro, not ready 
                paddr[2] = 0xff; // timeout for format 
                paddr[3] = 0xff; // copy of wd
                dcb->DSTATS = 0x1;
                pbiRequest->carry = 1;
                return copyRequired ? RES_FLAG_COPYOUT : RES_FLAG_COMPLETE;
            }
            
            if (dcb->DCOMND == 0x52 || dcb->DCOMND == 0xd2) {  // READ sector
                if (disk->read(paddr, sector) != (size_t)disk->specificSectorSize(sector)) {
                    dcb->DSTATS = 0;   // as a failed write: no garbage sector
                    pbiRequest->carry = 0;
                    return RES_FLAG_COMPLETE;
                }
                dcb->DSTATS = 0x1;
                pbiRequest->carry = 1;
                return copyRequired ? RES_FLAG_COPYOUT : RES_FLAG_COMPLETE;
            }
            if (dcb->DCOMND == 0x50 || dcb->DCOMND == 0xd0 || dcb->DCOMND == 0x57) {  // WRITE sector (ignore verify)
                if (copyRequired && (pbiRequest->req & REQ_FLAG_COPYIN) == 0) 
                    return RES_FLAG_NEED_COPYIN;
                int writeResult = disk->write(paddr, sector);
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
                paddr[0] = 0x28;
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
                PercomBlock *percom = (PercomBlock *)paddr;
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
                return copyRequired ? RES_FLAG_COPYOUT : RES_FLAG_COMPLETE;
            }
            if (dcb->DCOMND == 0x4f) {  // write percom block
                if (copyRequired && (pbiRequest->req & REQ_FLAG_COPYIN) == 0) 
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
        if (wifiInitialized)
            telnetServerRun();

        //sendHttpRequest();
        //connectToServer();

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
        printf("%d ticks to copy 16KB twice\n", elapsed);
        //sendHttpRequest();
        //connectToServer();
        yield();
#endif
    } else if (pbiRequest->cmd == PBICMD_SET_MONITOR_BOOT) {
        config.cartImage = "/SDX450_maxflash1.car.gz";
        config.save();
        atariCart.open(sio.resolve(config.cartImage).c_str());
        mmuOnChange(/*force==*/true);
    }
    return RES_FLAG_COMPLETE;
}

DRAM_ATTR int enableBusInTicks = 0;
PbiIocb *lastPbiReq;
DRAM_ATTR int requestLeaveHalted = 0;

#ifdef ATARI800_SIM
extern "C" CORE0_HOTSPOTS_ATTR void handlePbiRequest(PbiIocb *pbiRequest) {
#else
CORE0_HOTSPOTS_ATTR void handlePbiRequest(PbiIocb *pbiRequest) {
#endif

    pbiRequest->result = 0;
#if CONFIG_A8BB_SIDECAR_ENABLE
#if CONFIG_A8BB_SIDECAR_LOOPBACK
    sidecarHost.probe(5000); // simulate overhead of offloaded pbi call
#else
    // Requests for the Sidecar's drives go to sidecarHost instead of
    // handlePbiRequest2() (below).
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
    // PROFILE_MODE=GPIO: sample the live bus before handlePbiRequest2()'s
    // ScopedInterruptEnable detaches it; the 6502 is in the stack-resident
    // wait loop, so the sample sees board and native cycles both.
    PROFILE_BUS();
    pbiRequest->result |= handlePbiRequest2(pbiRequest);

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
#if !defined(CSIM) || defined(CSIM_BUS_CYCLE)   // csim: core 0 is a coroutine there
        // Wait until we know the 6502 is safely in the stack-resident program. 
        uint16_t addr = 0;
        uint32_t read = 0;
        uint32_t startTsc = XTHAL_GET_CCOUNT();
        static const DRAM_ATTR int sprogTimeout = 240000000;
        bmonTail = bmonHead;
#if 1 // disable stackprog wait 
        do {
            CSIM_HOOK();
            // Warning: this is a core0 ifetch hotspot, with a hand-tuned loop delay.
            while (bmonHead == bmonTail) {
                CSIM_HOOK();
                if (XTHAL_GET_CCOUNT() - startTsc > sprogTimeout) {
                    core0_exit(1, "-3 stackprog timeout");
                    return; // main loop will exit 
                }
#pragma GCC unroll 8
                for (int poll = 0; poll < 8 && bmonHead == bmonTail; ++poll) {
                    loopNezDelay(33);   // 30 passes + setup; was 15 two-cycle passes
                }
            }
            uint32_t bmon = bmonArray[bmonTail].r0;//REG_READ(SYSTEM_CORE_1_CONTROL_1_REG);
            bmonTail = (bmonTail + 1) & bmonArraySzMask; 
            uint32_t r0 = bmon >> bmonR0Shift;
            addr = r0 >> bus.addr.shift;
            read = r0 & bus.rw.mask;
        } while(read != 0 || addr != PBIROM_DUMMY_WRITE); 
        bmonTail = bmonHead;
#endif //#if 1
#endif // #ifdef CSIM

        // The 6502 is in the stack-resident loop and the bus is attached.
        if (pbiRequest->cmd == PBICMD_PROBE_BEGIN && memProbeActive)
            haltLineTest();
        pbiRequest->req = 0;
        atariRam[0x103] = 0x60; // modify stack_res_loop with RTS instruction 
    } else { 
        bmonTail = bmonHead;
        pbiRequest->req = 0;
    }
#ifdef HALT_6502
//    busyWait6502Ticks(100);
//    resume6502();
//    busyWait6502Ticks(5);
#endif
}
