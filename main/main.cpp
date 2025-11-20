#pragma GCC optimize("Ofast")
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
#include "spiffs.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include <deque>
#include <functional>
#include <algorithm>
#include <inttypes.h>
#include "soc/soc_caps.h"


#if CONFIG_FREERTOS_UNICORE != 1 
#error Arduino idf core must be compiled with CONFIG_FREERTOS_UNICORE=y and CONFIG_ESP_INT_WDT=n
#endif

#include <vector>
#include <string>
using std::vector;
using std::string;
#include "ascii2keypress.h"
#include "pinDefs.h"
#include "main.h" 
#ifndef ARDUINO
#include "arduinoLite.h"
#endif

#include "asmDefs.h"
#include "extMem.h"
#include "util.h" 
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#include "diskImage.h"
#include "diskFlash.h"
#include "diskSmb.h"
#include "cartridge.h"
#include "sysMonitor.h"
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


// boot SDX cartridge image - not working well enough to base stress tests on it 
//#define BOOT_SDX

#ifndef BOOT_SDX
#define RAMBO_XL256
#endif

spiffs *spiffs_fs = NULL;
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
DRAM_ATTR uint32_t lastVblankTsc = 0;

DRAM_ATTR DiskImage *atariDisks[8] = {NULL};

DRAM_ATTR LedRmt led;  

DRAM_ATTR int deferredInterrupt = 0, interruptRequested = 0, sysMonitorRequested = 0;

IRAM_ATTR void raiseInterrupt() {
    if ((d000Write[_0x1ff] & pbiDeviceNumMask) != pbiDeviceNumMask
        && (d000Write[_0x301] & 0x1) != 0
    ) {
        deferredInterrupt = 0;  
        d000Read[_0x1ff] = pbiDeviceNumMask;
        atariRam[PDIMSK] |= pbiDeviceNumMask;
        pinReleaseMask &= bus.irq_.maskInverse;
        for(auto &b : banksL1) 
            for (auto &ctrl : b.ctrl) 
                ctrl |= bus.irq_.mask;
        interruptRequested = 1;
    } else { 
        deferredInterrupt = 1;
    }
}

IRAM_ATTR void clearInterrupt() { 
    for(auto &b : banksL1) 
        for (auto &ctrl : b.ctrl) 
            ctrl &= bus.irq_.maskInverse;
    busyWait6502Ticks(2);
    pinReleaseMask |= bus.irq_.mask;
    interruptRequested = 0;
    busyWait6502Ticks(10);
    d000Read[_0x1ff] = 0x0;
    atariRam[PDIMSK] &= pbiDeviceNumMaskNOT;
}

IRAM_ATTR inline void bmonWaitCycles(int cycles) { 
    uint32_t stsc = XTHAL_GET_CCOUNT();
    for(int n = 0; n < cycles; n++) { 
        unsigned int oldHead = bmonHead;
        while(
            //XTHAL_GET_CCOUNT() - stsc < bmonTimeout && 
            oldHead == bmonHead) {
           // busyWait6502Ticks(1);
        }
    }
}

IRAM_ATTR void resume6502() { 
    bmonTail = bmonHead;    
    pinReleaseMask |= bus.halt_.mask;
    bmonWaitCycles(5);
    pinReleaseMask &= haltMaskNOT;
}

static DRAM_ATTR uint8_t savedD5Offset = 0;

IRAM_ATTR void enableBus() {
    //busWriteDisable = 0;
    //pinEnableMask = _0xffffffff;
    lastPageOffset[pageD5] = savedD5Offset;
    mmuState = mmuStateSaved; 
    busyWait6502Ticks(20);
}


// disableBus - 
// replace all banks[] pointers with pointers to dummy banks
// bus core1.cpp loop is still running and will continue to update banks[] pointers
// and may even mistakenly interpret mis-timed bus data as IO to cartctl region and
// update basicEnMux[] from cartBanks[].   Thus everything
// in not just banks[], but also basicEnMuxp[], osEnMux[], and cartBanks[] needs to be
// replaced with dummy banks.  


IRAM_ATTR void disableBus() {

    mmuStateSaved = mmuState;
    mmuState = mmuStateDisabled;    
    savedD5Offset = lastPageOffset[pageD5];

    //busWriteDisable = 1;
    //pinEnableMask = bus.halt_.mask;
    busyWait6502Ticks(2);
}

DRAM_ATTR static const int psram_sz =  32 * 1024;
DRAM_ATTR uint32_t *psram;
DRAM_ATTR uint32_t *psram_end;

DRAM_ATTR static const int testFreq = 1.78 * 1000000;//1000000;
DRAM_ATTR static const int lateThresholdTicks = 180 * 2 * 1000000 / testFreq;
static const DRAM_ATTR uint32_t halfCycleTicks = 240 * 1000000 / testFreq / 2;
const static DRAM_ATTR uint32_t bmonTimeout = 240 * 1000 * 10;

//  socat TCP-LISTEN:9999 - > file.bin
bool sendPsramTcp(const char *buf, int len, bool resetWdt = false) { 
#if 0
    //char *host = "10.250.250.240";
    char *host = "192.168.68.131";
    ////WiFi.begin("Station54", "Local1747"); host = "10.250.250.240";
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


DRAM_ATTR Hist2 profilers[numProfilers];
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
    if ((bmon & (bus.refresh_.mask << bmonR0Shift)) == 0)
        return;

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

volatile float foo = 1.01;
void IRAM_ATTR core0Loop() { 
    psramPtr = psram;

    // disable PBI ROM by corrupting it 
    //pbiROM[0x03] = 0xff;

    int bmonCaptureDepth = 0;

    const static DRAM_ATTR int prerollBufferSize = 64; // must be power of 2
    uint32_t prerollBuffer[prerollBufferSize]; 
    uint32_t prerollIndex = 0;
    PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];

    if (psram == NULL) {
        for(auto &t : bmonTriggers) t.count = 0;
    }
    busyWait6502Ticks(10000);
    // TODO: why is this needed?  seems to hint at a bug in core1 loop maybe impacting resume6502 
    // elsewhere.  Possibly figured out, see notes in resume6502()
    //REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.halt_.mask);
    resume6502();

    uint32_t bmon = 0;
    int repeatedBrokenRead = 0;
    bmonTail = bmonHead;

#if 0
    for(int j = 0; j < 2; j++) { 
        for(int n = 0; n < 240 * 1000000; n++) { 
            foo = foo * 2;
            ASM("nop;");
        }
    }
    return;

    uint32_t stsc = XTHAL_GET_CCOUNT();
    static const DRAM_ATTR uint32_t runtime = 10UL * 240 * 1000000;
    while(XTHAL_GET_CCOUNT() - stsc < runtime) { 
        for(int n = 0; n < 10000; n++) { 
            ASM("nop;");
        }
    }
    return;
#endif

    while(1) {
        uint32_t stsc = XTHAL_GET_CCOUNT();
        const static DRAM_ATTR uint32_t bmonTimeout = 240 * 1000 * 50;
        const static DRAM_ATTR uint32_t bmonMask = 0x2fffffff;
	while(
	    XTHAL_GET_CCOUNT() - stsc < bmonTimeout &&
	    true) {  
            while(
                XTHAL_GET_CCOUNT() - stsc < bmonTimeout && 
                bmonHead == bmonTail) {
                AsmNops<5>::generate(); 
            }
	    unsigned int bHead = bmonHead;
            if (bHead == bmonTail)
	            continue;
            
            //bmonMax = max((bHead - bmonTail) & bmonArraySzMask, bmonMax);
            bmon = bmonArray[bmonTail] & bmonMask;
        
            uint32_t r0 = bmon >> bmonR0Shift;

            uint16_t addr = (r0 & bus.addr.mask) >> bus.addr.shift;
            if ((r0 & bus.rw.mask) == 0) {
                uint32_t lastWrite = addr;
                if (lastWrite == _0xd301) { 
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
                    //pageNr(lastWrite) == pageNr_d1ff ||
                    false
                ) {
                    bmonWaitCycles(1); // don't know why it hangs without this 
                    resume6502();
                }
		        repeatedBrokenRead = 0;
            } else if ((r0 & bus.refresh_.mask) != 0) {
                uint16_t lastRead = addr;
                #ifdef FAKE_CLOCK
                if (addr == 0x180) break;
                #endif
                DRAM_ATTR static uint16_t lastLastRead = 0;
                if (lastRead == lastLastRead) { 
                    repeatedBrokenRead++;
                    if (repeatedBrokenRead > 30 && elapsedSec > 20) {
                        exitReason = sfmt("-4 6502 repeat reads %04x", lastRead);
                        exitFlag = true;
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
            }    
            //PROFILE_BMON((bHead - bmonTail) & bmonArraySzMask);
            bmonTail = (bmonTail + 1) & bmonArraySzMask;

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

        //restartHalted6502();

#ifdef FAKE_CLOCK
        // The above loop exits to here every 10ms or when an interesting address has been read 
        PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
        if (pbiRequest[0].req != 0) { 
            handlePbiRequest(&pbiRequest[0]); 
        } else if (pbiRequest[1].req != 0) { 
            handlePbiRequest(&pbiRequest[1]);
        }
#endif 

        if (1) { 
            static const DRAM_ATTR int keyTicks = 301 * 240 * 1000; // 150ms
            EVERYN_TICKS(keyTicks) { 
                if (simulatedKeyInput.available()) { 
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

        if (/*XXINT*/1 && (ioCount > 0)) {
            static DRAM_ATTR uint32_t ltsc = 0;
            if (config.interruptTicks > 0 && XTHAL_GET_CCOUNT() - ltsc > config.interruptTicks) { 
                ltsc = XTHAL_GET_CCOUNT();
                raiseInterrupt();
            }
        }

#if defined(FAKE_CLOCK) 
        if (1 && elapsedSec > 10) { //XXFAKEIO
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

#if 0 
                simulatedKeyInput.putKeys(DRAM_STR("-2:X\233"));

               if (!BUS_ANALYZER) {
                       //simulatedKeyInput.putKeys(DRAM_STR("DOS\233     D3:HELLO.EXE\233"));
                       simulatedKeyInput.putKeys(DRAM_STR("PAUSE 1\233E.\"J:X\"\233"));
               } else {
               	   uint16_t a = 1536;
                   for(int d : {

                    0x18, 0x8d, 0x11, 0x11, 0x90, 0xfb, 
			    	//0x78, 0xa9, 0x00, 0x8d, 0x0e, 0xd4, 0xad, 0x00, 0xd4, 0x18, 0x8d, 0x11, 0x11, 0x90, 0xfb, 
			        //0x78, 0xa9, 0x00, 0x8d, 0x0e, 0xd4, 0xad, 0x00, 0xd4, 0xad, 0x11, 0x11, 0x18, 0x90, 0xfa,
                   }) 
                       atariRam[a++] = d;
                   config.interruptTicks = -1;
                   config.wdTimeoutSec = config.ioTimeoutSec = 3600;
                   //atariRam[559] = 0;
                   simulatedKeyInput.putKeys(DRAM_STR("A=USR(1536)\233"));
               }

#endif
            }
            if (config.sysMonitorSec > 0 && (elapsedSec % config.sysMonitorSec) == 0) {  // XXSYSMON
                sysMonitorRequested = 1;
            }

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
                exitReason = "-1 Watchdog timeout";
                break;
            }
            if (config.ioTimeoutSec > 0 && elapsedSec - lastIoSec > config.ioTimeoutSec) { 
                exitReason = "-2 IO timeout";
                break;
            }
            if (1 && atariRam[_0x600] == 0xde) { 
                atariRam[_0x600] = 0;
                lastIoSec = elapsedSec;
                secondsWithoutWD = 0;
                ioCount++;
            }
#endif
            if (elapsedSec == 1) { 
                bmonMax = mmuChangeBmonMaxEnd = mmuChangeBmonMaxStart = 0;
                for(int i = 0; i < numProfilers; i++) profilers[i].clear();
            }
#if 0 // XXPOSTDUMP
            if (sizeof(bmonTriggers) >= sizeof(BmonTrigger) && elapsedSec == conf.runSec - 1) {
                bmonTriggers[0].value = bmonTriggers[0].mask = 0;
                bmonTriggers[0].depth = 1000;
                bmonTriggers[0].count = 1;
            }
#endif

            static DRAM_ATTR int runSec = config.runSec; // cache float conversion 
            if(elapsedSec > runSec && runSec > 0) {
                exitReason = "0 Specified run time reached";   
                break;
            }
            if(atariRam[754] == 0xef || atariRam[764] == 0xef) {
                exitReason = "1 Exit hotkey pressed";
                break;
            }
            if(atariRam[754] == 0xee || atariRam[764] == 0xee) {
                config.wdTimeoutSec = config.ioTimeoutSec = 1200;
                lastIoSec = elapsedSec;
                secondsWithoutWD = 0;
                atariRam[712] = 255;
            }
            if(exitFlag) {
                if (exitReason.length() == 0) 
                    exitReason = "2 Exit command received";
                break;
            }
        }
    }
}

void IFLASH_ATTR threadFunc(void *) { 
    printf("CORE0: threadFunc() start\n");
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);

    printf("opt.fakeClock %d runSec %d\n", opt.fakeClock, config.runSec);
    uint8_t chipid[6];
    esp_read_mac(chipid, ESP_MAC_WIFI_STA);
    mmuDebugPrint();
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
    uint32_t bmonCopy[bmonArraySz];
    for(int i = 0; i < bmonArraySz; i++) { 
        bmonCopy[i] = bmonArray[i];
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
    mmuDebugPrint();
#ifndef FAKE_CLOCK
    printf("bmonMax: %d mmuChangeBmonMaxEnd: %d mmuChangeBmonMaxStart: %d\n", bmonMax, mmuChangeBmonMaxEnd, mmuChangeBmonMaxStart);   
    printf("bmonArray:\n");
    uint16_t *addrHistogram = (uint16_t *)(uint8_t *)heap_caps_malloc(64 * 1024 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    printf("addrHistogram alloated %p\n", addrHistogram);
    if (addrHistogram != NULL) { 
        for(int n = 0; n < 64 * 1024; n++) addrHistogram[n] = 0;

        for(int i = 0; i < bmonArraySz; i++) { 
            uint32_t r0 = (bmonCopy[i]) >> bmonR0Shift;
            uint16_t addr = r0 >> bus.addr.shift;
            addrHistogram[addr]++;
 	
            uint8_t data = (bmonCopy[i] & 0xff);
            if (bmonExclude(bmonCopy[i])) continue;
            if (0) { // exhaustively print every bmon entry  
                char rw = (r0 & bus.rw.mask) != 0 ? 'R' : 'W';
                if ((r0 & bus.refresh_.mask) == 0) rw = 'F';
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


    uint64_t totalEvents = 0;
#if 0 
    for(int i = 0; i < profilers[1].maxBucket; i++) {
        totalEvents += profilers[1].buckets[i];
    }
    for(int i = 0; i < profilers[2].maxBucket; i++) {
        totalEvents += profilers[2].buckets[i];
    }
#endif
    printf("Total samples %" PRIu64 " implies %.2f sec sampling. Total reads %d\n",
        totalEvents, 1.0 * totalEvents / 1.8 / 1000000, ramReads);

    if (opt.histogram) {
        int first = profilers[0].maxBucket, last = 0;
        for (int c = 0; c < numProfilers; c++) { 
            for(int i = 1; i < profilers[c].maxBucket; i++) { 
                if (profilers[c].buckets[i] > 0 && i > last) last = i;
            }
            for(int i = profilers[c].maxBucket - 1; i > 0 ;i--) { 
                if (profilers[c].buckets[i] > 0 && i < first) first = i;
            }
        }

        for(int i = first; i <= last; i++) {
            printf("% 4d ", i);
            for(int c = 0; c < numProfilers; c++) {
                printf("% 12d ", profilers[c].buckets[i]);
            }
            printf(" HIST\n");
        }

        for (int c = 0; c < numProfilers; c++) {
            first = last = 0; 
            int total = 0;
            for(int i = 1; i < profilers[c].maxBucket; i++) { 
                if (profilers[c].buckets[i] > 0) last = i;
                total += profilers[c].buckets[i];
            }
            for(int i = profilers[c].maxBucket - 1; i > 0 ;i--) { 
                if (profilers[c].buckets[i] > 0) first = i;
            }
            yield();
            printf("channel %d: range %3d -%3d, jitter %3d, total %d  HIST\n", c, first, last, last - first, total);
        }
        uint64_t totalEvents = 0;
        for(int i = 0; i < profilers[0].maxBucket; i++)
            totalEvents += profilers[0].buckets[i];
        printf("Total samples %lld implies %.2f sec sampling\n", totalEvents, 1.0 * totalEvents / 1.8 / 1000000);
    }
    
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
                if ((r0 & bus.refresh_.mask) != 0) rw = 'F';
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
    structLogs->print();
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
    
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    int memReadErrors = 0;//(atariRam[0x609] << 24) + (atariRam[0x608] << 16) + (atariRam[0x607] << 16) + atariRam[0x606];
    printf("SUMMARY %-10.2f/%d e%d i%d d%d %s\n", millis()/1000.0, config.runSec, memReadErrors, 
        pbiInterruptCount, ioCount, exitReason.c_str());
    printf("pbi_init_complete %d, halts %d\n", pbiROM[0x20], haltCount);
    printf("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    printf("GPIO_EN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_ENABLE_REG),REG_READ(GPIO_ENABLE1_REG)); 
    printf("extMem swaps %d evictions %d d1ff %02x pinDr %08lx\n", 
        extMem.swapCount, extMem.evictCount, d000Write[0x1ff], 0L/*pinDriveMask*/);

    printf("lastPageWriteOffset[0xd5] %02x atariCart.bankA0 %02x banks[0xa0] %p cartBanks[1] %p atariCart.image[1].mmuData %p\n", 
        lastPageOffset[pageD5], atariCart.bankA0, mmuState.banks[bankA0], mmuState.cartBanks[1], atariCart.image != NULL ? &atariCart.image[1].mmuData : 0);

    printf("DONE %-10.2f %s\n", millis() / 1000.0, exitReason.c_str());
    delay(100);
    
    //ESP.restart();
    printf("CORE0 idle\n");
    while(1) { 
        //printf("CORE0 idle\n");
        delay(10); 
        yield();
    }
}

void *app_cpu_stack_ptr = NULL;
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

void IFLASH_ATTR startCpu1() {  
    if (REG_GET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN)) {
        printf("APP CPU is already running!\n");
        return;
    }

    if (!app_cpu_stack_ptr) {
        app_cpu_stack_ptr = heap_caps_malloc(512, MALLOC_CAP_INTERNAL);
    }

    DPORT_REG_WRITE(SYSTEM_CORE_1_CONTROL_1_REG, 0);
    DPORT_REG_WRITE(SYSTEM_CORE_1_CONTROL_0_REG, 0);
    DPORT_REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETING);
    DPORT_REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETING);

    ets_set_appcpu_boot_addr((uint32_t)&app_cpu_init);
    DPORT_REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
}


#include <fcntl.h>
#include "esp_err.h"
#include "esp_log.h"
extern "C" spiffs *spiffs_fs_by_label(const char *label); 

void setup() {
    for(auto i : gpios) pinMode(i, INPUT);
    pinMode(bus.halt_.pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(bus.halt_.pin, 0);
    //pinReleaseMask &= bus.halt_.maskInverse;

    led.init();
    led.write(20, 0, 0);
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

    //extMem.mapCompy192();
    //extMem.mapRambo256();
    //extMem.mapStockXL();
    //extMem.mapStockXE();
    extMem.mapNone();
    
    if (0) { 
        extMem.init(16, 4);
        extMem.mapNativeXe192(); //
    } else { 
        extMem.init(16, 0);
        extMem.mapNone();
    }
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
        printf("Partition size: total: %d, used: %d\n", total, used);
    }

    spiffs_fs = spiffs_fs_by_label(NULL); 

    //spiffs *sfs = NULL;
    //spiffs_file fd = SPIFFS_creat(spiffs_fs, "/xxx", 0);
    //spiffs_file fd = SPIFFS_open(spiffs_fs, "/d1.atr", SPIFFS_O_RDONLY, 0);
    
    //FILE *fd = fopen("/spiffs/d1.atr", "w");
    //while(1) {
    //    printf("open() returned %d, errno %d, spiffs_fs %x\n", (int)fd, errno, (int)spiffs_fs);
    //    delay(500);
    //}

    psram = (uint32_t *) heap_caps_aligned_alloc(64, psram_sz,  MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    psram_end = psram + (psram_sz / sizeof(psram[0]));
    if (psram != NULL)
        bzero(psram, psram_sz);

#ifdef BOOT_CONFIG
    config.load(BOOT_CONFIG);
#else
    config.load();
#endif
    printf("cartImage='%s'\n", config.cartImage.c_str());
    sysMonitor = new SysMonitor();
    fakeFile = new AtariIO();
    structLogs = new StructLogs();
    for(int d = 0; d < 8; d++) { 
        if (config.diskSpec[d].length() > 0) 
            atariDisks[d] = new DiskImageATR(spiffs_fs, config.diskSpec[d].c_str(), true);
    }
    if (atariDisks[2] == NULL)
        atariDisks[2] = new DiskStitchGeneric<SmbConnection>("smb://miner6.local/pub");
    atariCart.open(spiffs_fs, config.cartImage.c_str());
    if (atariCart.bankA0 >= 0) 
    	  pbiROM[DISABLE_BASIC - PBIROM_BASE] = 1;

    const vector<ProcFsConnection::ProcFsFile> procFsNodes = 
    {
        {   
            .name = "WIFI",
            .read =  [](string &buf) {},
            .write = [](const string &buf) {
                if (buf == "1") {
                    wifiRun();
                } else {
                    // TODO figure out how to shutdown WIFI 
                }
            },
        },
        {   
            .name = "FOOP",
            .read =  [](string &buf) {
                buf = "foop contents\233";
                printf("read from FOOP\n");
            },
            .write = [](const string &buf) {
                printf("write to FOOP: '%s'\n", buf.c_str());
            },
        },
    };


    if (atariDisks[3] == NULL)
        atariDisks[3] = new DiskProcFs(procFsNodes);
    //atariCart.open(spiffs_fs, "/Joust.rom");
    //atariCart.open(spiffs_fs, "/Edass.car");
    //atariCart.open(spiffs_fs, "/SDX450_maxflash1.car");

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

    if (opt.fakeClock) { // simulate clock signal 
        pinMode(bus.clock.pin, OUTPUT);
        digitalWrite(bus.clock.pin, 0);
        ledcAttachChannel(bus.clock.pin, testFreq, 1, 0);
#ifdef ARDUINO
        ledcWrite(pins.clock.pin, 1);
#else
        ledcWrite(0, 1);
#endif

        pinMode(bus.rw.pin, OUTPUT);
        digitalWrite(bus.rw.pin, 0);
        ledcAttachChannel(bus.rw.pin, testFreq / 8, 1, 2);
#ifdef ARDUINO
        ledcWrite(pins.read.pin, 1);
#else
        ledcWrite(2, 1);
#endif

        pinMode(bus.extDecode.pin, OUTPUT);
        digitalWrite(bus.extDecode.pin, 1);
        ledcAttachChannel(bus.extDecode.pin, testFreq / 2, 1, 4);
        ledcWrite(4, 1);

        // write 0xd1ff to address pins to simulate worst-case slowest address decode
        static const uint16_t testAddress = 0x0180;//0xd1ff;  
        for(int bit = 0; bit < 16; bit ++)
            pinMode(bus.addr.pin + bit, ((testAddress >> bit) & 1) == 1 ? INPUT_PULLUP : INPUT_PULLDOWN);

        //gpio_set_drive_capability((gpio_num_t)pins.clock.pin, GPIO_DRIVE_CAP_MAX);
        pinMode(bus.mpd.pin, INPUT_PULLDOWN);
        pinMode(bus.refresh_.pin, INPUT_PULLUP);
        //pinMode(pins.extDecode.pin, INPUT_PULLUP);
        pinMode(bus.extSel.pin, INPUT_PULLUP);
    }

    //pinDisable(bus.extDecode.pin);
    for(int i = 0; i < 1; i++) { 
        printf("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    }
    printf("freq %.4fMhz threshold %d halfcycle %d psram %p\n", 
        testFreq / 1000000.0, lateThresholdTicks, (int)halfCycleTicks, psram);

    gpio_matrix_in(bus.clock.pin, CORE1_GPIO_IN0_IDX, false);
    digitalWrite(bus.irq_.pin, 1);
    pinMode(bus.irq_.pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(bus.irq_.pin, 1);
    //initLed();
    led.write(0, 20, 0);

    pinMode(bus.irq_.pin, OUTPUT_OPEN_DRAIN);
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.irq_.mask);
    digitalWrite(bus.irq_.pin, 0);

    pinMode(bus.extSel.pin, OUTPUT_OPEN_DRAIN);
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.extSel.mask);
    digitalWrite(bus.extSel.pin, 0);
    
    pinMode(bus.halt_.pin, OUTPUT_OPEN_DRAIN);
    //REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.halt_.mask);
    //digitalWrite(bus.halt_.pin, 0);

    for(int i = 0; i < 8; i++) { 
        pinMode(bus.data.pin + i, OUTPUT); // TODO: Investigate OUTPUT_OPEN_DRAIN doesn't work, would enable larger page sizes if it did 
    }
    clearInterrupt();
    mmuInit();
    mmuDebugPrint();
    enableBus();
    startCpu1();
    busywait(.01);
    //threadFunc(NULL);
    xTaskCreatePinnedToCore(threadFunc, "core0Loop", 12 * 1024, NULL, 0, NULL, 0);
    while(1) { yield(); delay(1000); };
}
        
void loop() {
    while(1) { yield(); delay(1); }
}

static void app_cpu_main() {
    uint32_t oldint;
    //XT_INTEXC_HOOK oldnmi = _xt_intexc_hooks[XCHAL_NMILEVEL];
    //_xt_intexc_hooks[XCHAL_NMILEVEL] = my_nmi;  // saves 5 cycles, could save more 
    //
    ASM("rsil %0, 15" : "=r"(oldint) : : );
    iloop_pbi();
    while(1) {}
}

#ifdef CSIM
class SketchCsim : public Csim_Module {
    public:
    void setup() {HTTPClient::csim_onPOST("http://.*/log", 
        [](const char *url, const char *hdr, const char *data, string &result) {
 	return 200; }); }
    string dummy;
    void parseArg(char **&a, char **la) override { if (strcmp(*a, "--dummy") == 0) dummy = *(++a); }
    void loop() override {}
} sketchCsim;
#endif
 


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

//            # "mmu.cpp" "bmon.cpp" "pbi.cpp" "sio.cpp" "cio.cpp" "extMem.cpp" "arduinoLite.cpp" "cartridge.cpp" "core1.cpp" 
#include "mmu.cpp"
#include "bmon.cpp"
#include "pbi.cpp"
#include "sio.cpp"
#include "cio.cpp"
#include "extMem.cpp"
#include "arduinoLite.cpp"
#include "cartridge.cpp"
#include "core1.cpp"
