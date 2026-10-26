#ifdef CSIM
#include "csim.h"
#endif

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
#include "sysConfig.h"
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
        for(int n = 0; n < sizeof(atariDisks)/sizeof(atariDisks[0]); n++) {
            // TMP disable until better error handling 
            //if (atariDisks[n] != NULL) atariDisks[n]->start();
        }
    }
}

struct ScopedBlinkLED { 
    DRAM_ATTR static uint8_t cur[3];// = {0};
    uint8_t prev[3];
    ScopedBlinkLED(uint8_t *set) {
        for(int n = 0; n < sizeof(cur); n++) prev[n] = cur[n];
        for(int n = 0; n < sizeof(cur); n++) cur[n] = set[n];
#if !PROFILE_GPIO_ENABLED
        led.write(set[0], set[1], set[2]); 
#endif
    }
    ~ScopedBlinkLED() {  
#if !PROFILE_GPIO_ENABLED
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
    DRAM_ATTR static int lastPrint = -999;
    if (elapsedSec - lastPrint >= 2) {
        handleSerialInput();
        lastPrint = elapsedSec;
        DRAM_ATTR static int lastIoCount = 0;
        printf(DRAM_STR("time %02d:%02d:%02d iocount: %8d (%3d) irqcount %d http %d "
            "halts %d evict %d/%d i2s_dma_eofs %u\n"),
            elapsedSec/3600, (elapsedSec/60)%60, elapsedSec%60, ioCount,  
            ioCount - lastIoCount, 
            pbiInterruptCount, httpRequests, haltCount, extMem.evictCount, extMem.swapCount,
            static_cast<unsigned>(i2sDmaPocEofCount));
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

namespace {

constexpr uint32_t PBI_IDE_SECTOR_SIZE = 512;
constexpr uint8_t PBI_IDE_MOUNTED = 0x02;
constexpr size_t PBI_IDE_MAX_TRANSFER = 1024;

struct PbiIdeMountedDrive {
    bool mounted;
    uint16_t partitionId;
    uint32_t startSector;
    uint32_t sectorCount;
};

DRAM_ATTR PbiIdeMountedDrive pbiIdeMountedDrives[8] = {};

DiskImage *pbiIdeBackingDisk() {
    DiskImage *disk = atariDisks[7];
    return disk != nullptr && disk->valid() ? disk : nullptr;
}

uint64_t pbiIdeBackingSize(DiskImage *disk) {
    if (disk == nullptr || disk->sectorCount() <= 0)
        return 0;
    const uint64_t size = disk->imageSize();
    return size <= 0x7fffffffU ? size : 0;
}

bool pbiIdeReadBytes(DiskImage *disk, uint64_t offset, uint8_t *buffer, size_t length) {
    const uint64_t imageSize = pbiIdeBackingSize(disk);
    if (disk == nullptr || offset > imageSize || length > imageSize - offset)
        return false;

    size_t copied = 0;
    while (copied < length) {
        const uint64_t current = offset + copied;
        const int sector = disk->offsetToSector(static_cast<int>(current));
        const int sectorOffset = disk->sectorToOffset(sector);
        const int sectorSize = disk->specificSectorSize(sector);
        if (sector < 1 || sectorSize <= 0 || sectorSize > static_cast<int>(PBI_IDE_SECTOR_SIZE) ||
            current < static_cast<uint64_t>(sectorOffset) ||
            current >= static_cast<uint64_t>(sectorOffset) + sectorSize)
            return false;

        uint8_t sectorBuffer[PBI_IDE_SECTOR_SIZE];
        if (disk->read(sectorBuffer, sector) < static_cast<size_t>(sectorSize))
            return false;
        const size_t inSector = static_cast<size_t>(current - sectorOffset);
        const size_t chunk = min(length - copied, static_cast<size_t>(sectorSize) - inSector);
        memcpy(buffer + copied, sectorBuffer + inSector, chunk);
        copied += chunk;
    }
    return true;
}

bool pbiIdeWriteBytes(DiskImage *disk, uint64_t offset, const uint8_t *buffer, size_t length) {
    const uint64_t imageSize = pbiIdeBackingSize(disk);
    if (disk == nullptr || offset > imageSize || length > imageSize - offset)
        return false;

    size_t copied = 0;
    while (copied < length) {
        const uint64_t current = offset + copied;
        const int sector = disk->offsetToSector(static_cast<int>(current));
        const int sectorOffset = disk->sectorToOffset(sector);
        const int sectorSize = disk->specificSectorSize(sector);
        if (sector < 1 || sectorSize <= 0 || sectorSize > static_cast<int>(PBI_IDE_SECTOR_SIZE) ||
            current < static_cast<uint64_t>(sectorOffset) ||
            current >= static_cast<uint64_t>(sectorOffset) + sectorSize)
            return false;

        uint8_t sectorBuffer[PBI_IDE_SECTOR_SIZE];
        if (disk->read(sectorBuffer, sector) < static_cast<size_t>(sectorSize))
            return false;
        const size_t inSector = static_cast<size_t>(current - sectorOffset);
        const size_t chunk = min(length - copied, static_cast<size_t>(sectorSize) - inSector);
        memcpy(sectorBuffer + inSector, buffer + copied, chunk);
        if (disk->write(sectorBuffer, sector) < sectorSize)
            return false;
        copied += chunk;
    }
    return true;
}

uint32_t pbiIdeReadLe32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

void pbiIdeWriteLe32(uint8_t *data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}

void pbiIdeWriteAtaWord(uint8_t *data, size_t word, uint16_t value) {
    data[word * 2] = static_cast<uint8_t>(value);
    data[word * 2 + 1] = static_cast<uint8_t>(value >> 8);
}

void pbiIdeWriteAtaString(uint8_t *data, size_t offset, size_t length, const char *value) {
    const size_t valueLength = strlen(value);
    for (size_t i = 0; i < length; i += 2) {
        const uint8_t first = i < valueLength ? static_cast<uint8_t>(value[i]) : ' ';
        const uint8_t second = i + 1 < valueLength ? static_cast<uint8_t>(value[i + 1]) : ' ';
        data[offset + i] = second;
        data[offset + i + 1] = first;
    }
}

bool pbiIdeMountPartition(const uint8_t *mcb, DiskImage *disk) {
    unsigned int drive = mcb[1];
    if (mcb[0] != PBI_IDE_MOUNTED)
        return false;
    if (drive >= '1' && drive <= '8')
        drive -= '0';
    const uint32_t partitionId = pbiIdeReadLe32(mcb + 2);
    if (drive < 1 || drive > 8 || partitionId < 1 || partitionId > 4)
        return false;

    uint8_t mbr[PBI_IDE_SECTOR_SIZE];
    if (!pbiIdeReadBytes(disk, 0, mbr, sizeof(mbr)) || mbr[510] != 0x55 || mbr[511] != 0xaa)
        return false;
    const size_t entry = 0x1be + (partitionId - 1) * 16;
    const uint32_t startSector = pbiIdeReadLe32(mbr + entry + 8);
    const uint32_t sectorCount = pbiIdeReadLe32(mbr + entry + 12);
    const uint64_t diskSectors = pbiIdeBackingSize(disk) / PBI_IDE_SECTOR_SIZE;
    if (mbr[entry + 4] == 0 || sectorCount == 0 || startSector >= diskSectors ||
        sectorCount > diskSectors - startSector)
        return false;

    PbiIdeMountedDrive &mounted = pbiIdeMountedDrives[drive - 1];
    mounted.mounted = true;
    mounted.partitionId = static_cast<uint16_t>(partitionId);
    mounted.startSector = startSector;
    mounted.sectorCount = sectorCount;
    return true;
}

void pbiIdeFillDiskInfo(uint8_t *buffer, size_t length, const PbiIdeMountedDrive *mounted,
                        uint32_t diskSectors) {
    memset(buffer, 0, length);
    buffer[0] = 1;
    buffer[1] = 0;
    buffer[2] = 1;
    if (mounted != nullptr) {
        buffer[3] = static_cast<uint8_t>(mounted->partitionId);
        buffer[4] = static_cast<uint8_t>(mounted->partitionId >> 8);
        buffer[6] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE);
        buffer[7] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE >> 8);
        buffer[8] = static_cast<uint8_t>(mounted->sectorCount);
        buffer[9] = static_cast<uint8_t>(mounted->sectorCount >> 8);
        buffer[10] = static_cast<uint8_t>(mounted->sectorCount >> 16);
        buffer[11] = static_cast<uint8_t>(mounted->sectorCount >> 24);
        buffer[12] = static_cast<uint8_t>(mounted->startSector);
        buffer[13] = static_cast<uint8_t>(mounted->startSector >> 8);
        buffer[14] = static_cast<uint8_t>(mounted->startSector >> 16);
        buffer[15] = static_cast<uint8_t>(mounted->startSector >> 24);
    } else {
        buffer[3] = 3;
        buffer[6] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE);
        buffer[7] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE >> 8);
        pbiIdeWriteLe32(buffer + 8, diskSectors);
        memcpy(buffer + 0x10, "GENERIC 16M SSD", sizeof("GENERIC 16M SSD"));
    }
}

bool pbiIdeSetPercom(uint8_t *buffer, size_t length, uint32_t sectorCount) {
    if (length != 12 || sectorCount > 0xffff)
        return false;
    memset(buffer, 0, 12);
    buffer[0] = 1;
    buffer[1] = 3;
    buffer[2] = static_cast<uint8_t>(sectorCount >> 8);
    buffer[3] = static_cast<uint8_t>(sectorCount);
    buffer[4] = 1;
    buffer[5] = 4;
    buffer[6] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE >> 8);
    buffer[7] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE);
    buffer[8] = 0xff;
    return true;
}

int handlePbiIdeDcb(PbiIocb *request, AtariDCB *dcb) {
    const uint8_t device = dcb->DDEVIC;
    const uint8_t command = dcb->DCOMND;
    const uint8_t unit = dcb->DUNIT;
    const uint16_t address = static_cast<uint16_t>(dcb->DBUFLO | (dcb->DBUFHI << 8));
    const uint16_t length = static_cast<uint16_t>(dcb->DBYTLO | (dcb->DBYTHI << 8));
    const uint32_t sector = static_cast<uint32_t>(dcb->DAUX1) |
        (static_cast<uint32_t>(dcb->DAUX2) << 8) |
        (device == 0x20 ? 0 : static_cast<uint32_t>(atariRam[0x30c]) << 16) |
        (device == 0x20 ? 0 : static_cast<uint32_t>(atariRam[0x30d]) << 24);
    const bool mountedLogicalDrive = device == 0x31 && unit >= 1 && unit <= 8 &&
        pbiIdeMountedDrives[unit - 1].mounted;
    const bool recognizedDevice = device == 0xa0 || device == 0xb1 || device == 0x20 ||
        (device == 0x31 && command == 0x6e) || mountedLogicalDrive;
    if (!recognizedDevice)
        return -1;
    DiskImage *disk = pbiIdeBackingDisk();
    if (disk == nullptr)
        return -1;

    request->copybuf = address;
    request->copylen = length;
    if (static_cast<uint32_t>(address) + length > 0x10000U) {
        dcb->DSTATS = 0;
        request->carry = 0;
        return RES_FLAG_COMPLETE;
    }
    uint8_t *buffer = mmuCheckRangeMapped(mmuStateSaved, address, length);
    const bool copyRequired = buffer == nullptr;
    if (copyRequired && length > PBI_IDE_MAX_TRANSFER) {
        dcb->DSTATS = 0;
        request->carry = 0;
        return RES_FLAG_COMPLETE;
    }
    if (copyRequired)
        buffer = &pbiROM[0x400];

    auto finish = [&](bool success, bool copyout = false) {
        dcb->DSTATS = success ? 1 : 0;
        request->carry = success ? 1 : 0;
        return success && copyRequired && copyout ? RES_FLAG_COPYOUT : RES_FLAG_COMPLETE;
    };
    auto needsCopyIn = [&]() {
        if (copyRequired && (request->req & REQ_FLAG_COPYIN) == 0) {
            request->result |= RES_FLAG_NEED_COPYIN;
            return true;
        }
        return false;
    };

    const uint32_t diskSectors = static_cast<uint32_t>(pbiIdeBackingSize(disk) / PBI_IDE_SECTOR_SIZE);
    PbiIdeMountedDrive *mounted = nullptr;
    bool physical = device == 0xa0 || device == 0x20;
    bool logicalDevice = device == 0xb1 || device == 0x31;
    uint8_t drive = unit;
    if (logicalDevice) {
        if (device == 0x31) {
            if (command == 0x6e)
                drive = 0;
        }
        if (drive >= 1 && drive <= 8 && pbiIdeMountedDrives[drive - 1].mounted)
            mounted = &pbiIdeMountedDrives[drive - 1];
        else if (command != 0x6e)
            return finish(false);
    }
    if (physical && unit != 1)
        return finish(false);

    switch (command) {
    case 0x4e:
        return finish(pbiIdeSetPercom(buffer, length, mounted ? mounted->sectorCount : diskSectors), true);
    case 0x6e:
        if (physical) {
            if (length != 512)
                return finish(false);
            pbiIdeFillDiskInfo(buffer, length, nullptr, diskSectors);
        } else {
            if (length != 64)
                return finish(false);
            PbiIdeMountedDrive *info = nullptr;
            if (device == 0x31) {
                const uint8_t partitionId = static_cast<uint8_t>(dcb->DUNIT);
                for (auto &candidate : pbiIdeMountedDrives) {
                    if (candidate.mounted && candidate.partitionId == partitionId) {
                        info = &candidate;
                        break;
                    }
                }
            } else {
                info = mounted;
            }
            if (info == nullptr)
                return finish(false);
            pbiIdeFillDiskInfo(buffer, length, info, diskSectors);
        }
        return finish(true, true);
    case 0x4d: {
        if (length != 6)
            return finish(false);
        if (needsCopyIn())
            return RES_FLAG_NEED_COPYIN;
        if ((buffer[0] & 0x20) != 0) {
            uint16_t driveMap = 0;
            for (size_t i = 0; i < 8; ++i) {
                if (pbiIdeMountedDrives[i].mounted)
                    driveMap |= static_cast<uint16_t>(1U << i);
            }
            buffer[0] = static_cast<uint8_t>(driveMap);
            buffer[1] = static_cast<uint8_t>(driveMap >> 8);
            return finish(true, true);
        }
        return finish(pbiIdeMountPartition(buffer, disk));
    }
    case 0xec:
        if (!physical || length != 512)
            return finish(false);
        memset(buffer, 0, 512);
        pbiIdeWriteAtaWord(buffer, 1, 64);
        pbiIdeWriteAtaWord(buffer, 3, 16);
        pbiIdeWriteAtaWord(buffer, 6, 32);
        pbiIdeWriteAtaString(buffer, 20, 20, "00000000000000000001");
        pbiIdeWriteAtaString(buffer, 54, 40, "GENERIC 16MB SSD");
        pbiIdeWriteAtaWord(buffer, 49, 0x0200);
        pbiIdeWriteAtaWord(buffer, 53, 1);
        pbiIdeWriteAtaWord(buffer, 54, 64);
        pbiIdeWriteAtaWord(buffer, 55, 16);
        pbiIdeWriteAtaWord(buffer, 56, 32);
        pbiIdeWriteAtaWord(buffer, 57, static_cast<uint16_t>(diskSectors));
        pbiIdeWriteAtaWord(buffer, 58, static_cast<uint16_t>(diskSectors >> 16));
        pbiIdeWriteAtaWord(buffer, 59, 0x0120);
        pbiIdeWriteAtaWord(buffer, 60, static_cast<uint16_t>(diskSectors));
        pbiIdeWriteAtaWord(buffer, 61, static_cast<uint16_t>(diskSectors >> 16));
        return finish(true, true);
    case 0x53:
        if (length != 4)
            return finish(false);
        buffer[0] = 0x20;
        buffer[1] = buffer[2] = buffer[3] = 0xff;
        return finish(true, true);
    case 0x52:
    case 0xd2: {
        const uint64_t lba = mounted ? static_cast<uint64_t>(mounted->startSector) + sector - 1 : sector;
        const uint64_t count = length / PBI_IDE_SECTOR_SIZE;
        if (length == 0 || length % PBI_IDE_SECTOR_SIZE != 0 ||
            (!physical && sector == 0) || lba >= diskSectors || count > diskSectors - lba ||
            (mounted != nullptr && (sector - 1 >= mounted->sectorCount ||
             count > mounted->sectorCount - (sector - 1))) ||
            !pbiIdeReadBytes(disk, lba * PBI_IDE_SECTOR_SIZE, buffer, length))
            return finish(false);
        return finish(true, true);
    }
    case 0x50:
    case 0xd0:
    case 0x57:
    case 0xd7: {
        const uint64_t lba = mounted ? static_cast<uint64_t>(mounted->startSector) + sector - 1 : sector;
        const uint64_t count = length / PBI_IDE_SECTOR_SIZE;
        if (length == 0 || length % PBI_IDE_SECTOR_SIZE != 0 ||
            (!physical && sector == 0) || lba >= diskSectors || count > diskSectors - lba ||
            (mounted != nullptr && (sector - 1 >= mounted->sectorCount ||
             count > mounted->sectorCount - (sector - 1))))
            return finish(false);
        if (needsCopyIn())
            return RES_FLAG_NEED_COPYIN;
        return finish(pbiIdeWriteBytes(disk, lba * PBI_IDE_SECTOR_SIZE, buffer, length));
    }
    default:
        return finish(false);
    }
}

}

IRAM_ATTR __attribute__((noinline)) int handlePbiRequest2(PbiIocb *pbiRequest) {     
    //if (config.haltAvailable == false) pinReleaseMask |= bus.halt_.mask;
    //busyWait6502Ticks(5000);  // why does this cause immediate 13.8sec stackprog timeouts?
    SCOPED_INTERRUPT_ENABLE(pbiRequest);
    
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
    structLogs->pbi.add(*pbiRequest);
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
        structLogs->dcb.add(*dcb);
        if (0) { 
            printf(DRAM_STR("DCB: "));
            StructLog<AtariDCB>::printEntry(*dcb);
            fflush(stdout);
        }
        const int ideResult = handlePbiIdeDcb(pbiRequest, dcb);
        if (ideResult >= 0) {
            lastIoSec = elapsedSec;
            ioCount++;
            return ideResult;
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

            uint8_t *paddr = mmuCheckRangeMapped(mmuStateSaved, addr, dbyt);
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
        config.cartImage = "/SDX450_maxflash1.car.gz";
        config.save();
        atariCart.open(spiffs_fs, config.cartImage.c_str());
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
#ifndef CSIM
        // Wait until we know the 6502 is safely in the stack-resident program. 
        uint16_t addr = 0;
        uint32_t refresh = 0;
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
                    exitReason = sfmt("-3 stackprog timeout");
                    exitFlag = true;
                    return; // main loop will exit 
                }
#pragma GCC unroll 8
                for (int poll = 0; poll < 8 && bmonHead == bmonTail; ++poll) {
                    loopNezDelay(15);
                }
            }
            uint32_t bmon = bmonArray[bmonTail].r0;//REG_READ(SYSTEM_CORE_1_CONTROL_1_REG);
            bmonTail = (bmonTail + 1) & bmonArraySzMask; 
            uint32_t r0 = bmon >> bmonR0Shift;
            addr = r0 >> bus.addr.shift;
            refresh = r0 & bus.refresh_.mask;
        } while(refresh == 0 || addr != 0x103); 
        bmonTail = bmonHead;
#endif //#if 1
#endif // #ifdef CSIM
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
