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
#include "spiffs.h"
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
#include "sfmt.h"
#include "extMem.h"
#include "diskImage.h"
#include "cartridge.h"
#include "sysMonitor.h"
#include "ascii2keypress.h"
#include "cio.h"
#include "log.h"
#include "led.h"
#include "util.h"
#include "telnet.h"
#include "arduinoLite.h"

using std::min;
using std::max;

void sendHttpRequest();
void connectWifi();
void connectToServer();
void start_webserver(void);

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
        ASM("rsil %0, 15" : "=r"(oldint) : : );
        disableCore0WDT();
        busyWait6502Ticks(2000); // wait for core1 to stabilize again 
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
    if (wifiInitialized == false) { 
        connectWifi(); // 82876 bytes 
        start_webserver();  //12516 bytes 
        //smbReq();
        startTelnetServer();
        wifiInitialized = true;
        for(int n = 0; n < sizeof(atariDisks)/sizeof(atariDisks[0]); n++) {
            // TMP disable until better error handling 
            //if (atariDisks[n] != NULL) atariDisks[n]->start();
        }
    } else { 
        telnetServerRun();
    }
}

struct ScopedBlinkLED { 
    DRAM_ATTR static uint8_t cur[3];// = {0};
    uint8_t prev[3];
    ScopedBlinkLED(uint8_t *set) {
        for(int n = 0; n < sizeof(cur); n++) prev[n] = cur[n];
        for(int n = 0; n < sizeof(cur); n++) cur[n] = set[n];
        led.write(set[0], set[1], set[2]); 
    }
    ~ScopedBlinkLED() {  
        led.write(prev[0], prev[1], prev[2]); 
        for(int n = 0; n < sizeof(cur); n++) cur[n] = prev[n];
    }
};
uint8_t ScopedBlinkLED::cur[3];
#define SCOPED_BLINK_LED(a,b,c) ScopedBlinkLED blink((uint8_t []){a,b,c});

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
                exitFlag = x;
            } else if (sscanf(line, DRAM_STR("screen %c"), &x) == 1) {
                dumpScreenToSerial(x);
            }
        });
    }
}

void IFLASH_ATTR dumpScreenToSerial(char tag, uint8_t *mem/*= NULL*/) {
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    if (mem == NULL) {
        mem = mmuCheckRangeMapped(savmsc, 24 * 40);
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
}

uint8_t *mappedElseCopyIn(PbiIocb *pbiRequest, uint16_t addr, uint16_t len) { 
    uint8_t *rval = mmuCheckRangeMapped(addr, len);
    if (rval != NULL)
        return rval;
    
    if ((pbiRequest->req & REQ_FLAG_COPYIN) == 0) {
        // TODO assert(len < REQ_MAX_COPYLEN)
        pbiRequest->copybuf = addr;
        pbiRequest->copylen = len;
        pbiRequest->result = RES_FLAG_NEED_COPYIN;
        return NULL;
    }
    // TODO assert(pbiRequest->copybuf == addr)
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
    DRAM_ATTR static int lastPrint = -999;
    if (elapsedSec - lastPrint >= 2) {
        handleSerialInput();
        lastPrint = elapsedSec;
        DRAM_ATTR static int lastIoCount = 0;
        printf(DRAM_STR("time %02d:%02d:%02d iocount: %8d (%3d) irqcount %d http %d "
            "halts %d evict %d/%d\n"), 
            elapsedSec/3600, (elapsedSec/60)%60, elapsedSec%60, ioCount,  
            ioCount - lastIoCount, 
            pbiInterruptCount, httpRequests, haltCount, extMem.evictCount, extMem.swapCount);
        fflush(stdout);
        if (BUS_ANALYZER && elapsedSec > 12) { 
            printf("DONE\n");
        }
        fflush(stdout);
        lastIoCount = ioCount;
    }
    if (elapsedSec - lastScreenShot >= 90) {
        handleSerialInput();
        dumpScreenToSerial('Y');
        fflush(stdout);
        lastScreenShot = elapsedSec;
    }
}

IRAM_ATTR int handlePbiRequest2(PbiIocb *pbiRequest) {     
#if 0
    if (pbiRequest->cmd == 1) { // open
        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 1; 
        ioCount++;
        basicEnBankMux[0] = cartBanks[0];

	    return RES_FLAG_COMPLETE;
    } else if (pbiRequest->cmd == 2) { //close
        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 1; 
        ioCount++;
	    return RES_FLAG_COMPLETE;
    }
#endif

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

    SCOPED_INTERRUPT_ENABLE(pbiRequest);
    //ScopedPrintStructLog ps(pbiRequest);
    structLogs->pbi.add(*pbiRequest);
    handleSerialIO(pbiRequest);

    AtariIOCB *iocb = (AtariIOCB *)&atariRam[AtariDef.IOCB0 + pbiRequest->x]; // todo validate x bounds
    //pbiRequest->y = 1; // assume success
    //pbiRequest->carry = 0; // assume fail 
    if (pbiRequest->cmd == 1) { // open
        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 0; // assume fail 
        uint16_t addr = ((uint16_t )atariMem.ziocb->ICBAH) << 8 | atariMem.ziocb->ICBAL;
        int dbyt = (atariMem.ziocb->ICBLH << 8) + atariMem.ziocb->ICBLL;
        uint8_t *paddr = mappedElseCopyIn(pbiRequest, addr, 32);
        if (paddr == NULL)
            return RES_FLAG_NEED_COPYIN;
        char filename[33] = {0};
        for(int i = 0; i < sizeof(filename) - 1; i++) { 
            uint8_t ch = paddr[i];
            if (ch == 155) break;
            filename[i] = ch;    
        } 
        printf("AtariIO::open('%s') dbyt=%d IOCB: ", filename, dbyt);
        StructLog<AtariIOCB>::printEntry(*atariMem.ziocb);
        fakeFile->open(filename);
        structLogs->opens.add(filename);
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 2) { // close
        pbiRequest->y = 1; 
        fakeFile->close();
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 3) { // get
        pbiRequest->y = 1; 
        int c = fakeFile->get();
        if (c < 0) 
            pbiRequest->y = 136;
        else
            pbiRequest->a = c; 
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 4) { // put
        pbiRequest->y = 1; 
        if (fakeFile->put(pbiRequest->a, pbiRequest) < 0)
            pbiRequest->y = 136;
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
        structLogs->dcb.add(*dcb);
        if (0) { 
            printf(DRAM_STR("DCB: "));
            StructLog<AtariDCB>::printEntry(*dcb);
            fflush(stdout);
        }
        if (dcb->DDEVIC == 0x31 && dcb->DUNIT >= 1 && dcb->DUNIT < 9) {  // Device D1:
            DiskImage *disk = atariDisks[dcb->DUNIT - 1]; 
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

            uint8_t *paddr = mmuCheckRangeMapped(addr, dbyt);
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
                disk->read(paddr, sector);
                dcb->DSTATS = 0x1;
                pbiRequest->carry = 1;
                return copyRequired ? RES_FLAG_COPYOUT : RES_FLAG_COMPLETE;
            }
            if (dcb->DCOMND == 0x50 || dcb->DCOMND == 0xd0) {  // WRITE sector
                if (copyRequired && (pbiRequest->req & REQ_FLAG_COPYIN) == 0) 
                    return RES_FLAG_NEED_COPYIN;
                disk->write(paddr, sector);   
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
        //printf("ISR\n");
        // only do this once, don't try and re-map and follow screen mem around if it moves
        DRAM_ATTR static bool screenMemMapped = false;
        if (!screenMemMapped) { 
            int savmsc = (atariRam[89] << 8) + atariRam[88];
            int len = 20 * 40;
            if (mmuCheckRangeMapped(savmsc, len) == NULL) {
                int numPages = pageNr(savmsc + len) - pageNr(savmsc) + 1;
                if (screenMem == NULL) { 
                    screenMem = (uint8_t *)heap_caps_malloc(numPages * pageSize, MALLOC_CAP_INTERNAL);
                    assert(screenMem != NULL);
                }
                if(!pbiCopyAndMapPagesIntoBasemem(pbiRequest, pageNr(savmsc), numPages, screenMem))
                    return RES_FLAG_NEED_COPYIN;
                dumpScreenToSerial('M');
            }
            screenMemMapped = true;
        }
        if (/*elapsedSec > 20 || */wifiInitialized) 
            wifiRun();

        //sendHttpRequest();
        //connectToServer();

    } else if (pbiRequest->cmd == 11) { // system monitor
        SCOPED_BLINK_LED(0,20,0);
        sysMonitorRequested = 0;
        sysMonitor->pbi(pbiRequest);
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
        config.cartImage = "/SDX450_maxflash1.car";
        config.save();
        atariCart.open(spiffs_fs, config.cartImage.c_str());
        mmuOnChange(/*force==*/true);
    }
    return RES_FLAG_COMPLETE;
}

DRAM_ATTR int enableBusInTicks = 0;
PbiIocb *lastPbiReq;
DRAM_ATTR int requestLeaveHalted = 0;

IRAM_ATTR void handlePbiRequest(PbiIocb *pbiRequest) {  
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
    pbiRequest->result = 0;
    pbiRequest->result |= handlePbiRequest2(pbiRequest);

    if (pbiRequest->consol == 1 || pbiRequest->kbcode == 0xe5 || sysMonitorRequested)  {
        pbiRequest->result |= RES_FLAG_MONITOR;
    }
    bmonTail = bmonHead;
#ifdef HALT_6502
    busyWait6502Ticks(100);
    resume6502();
    busyWait6502Ticks(5);
#endif
    bmonTail = bmonHead;
    if ((pbiRequest->req & REQ_FLAG_STACKWAIT) != 0) {
        // Wait until we know the 6502 is safely in the stack-resident program. 
        uint16_t addr = 0;
        uint32_t refresh = 0;
        uint32_t startTsc = XTHAL_GET_CCOUNT();
        static const DRAM_ATTR int sprogTimeout = 240000000;
        bmonTail = bmonHead;
#if 1 // disable stackprog wait 
        do {
            while(bmonHead == bmonTail) { 
                if (XTHAL_GET_CCOUNT() - startTsc > sprogTimeout) {
                    exitReason = sfmt("-3 stackprog timeout, stackprog 0x%02x", (int)pbiRequest->stackprog);
                    exitFlag = true;
                    return; // main loop will exit 
                }
            }
            uint32_t bmon = bmonArray[bmonTail];//REG_READ(SYSTEM_CORE_1_CONTROL_1_REG);
            bmonTail = (bmonTail + 1) & bmonArraySzMask; 
            uint32_t r0 = bmon >> bmonR0Shift;
            addr = r0 >> bus.addr.shift;
            refresh = r0 & bus.refresh_.mask;
        } while(refresh == 0 || addr != 0x100 + pbiRequest->stackprog - 2); // stackprog is only low-order byte
        bmonTail = bmonHead;
#endif 
        pbiRequest->req = 0;
        atariRam[0x100 + pbiRequest->stackprog - 2] = 0;
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
