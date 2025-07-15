#pragma GCC optimize("O1")
#ifndef CSIM
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
#include <driver/gpio.h>
#include <driver/dedic_gpio.h>
#include <freertos/xtensa_timer.h>
#include <freertos/xtensa_rtos.h>

#include "driver/spi_master.h"
#include "rom/ets_sys.h"
#include "soc/dport_access.h"
#include "soc/system_reg.h"

//#include <esp_spi_flash.h>
#include "esp_partition.h"
#include "esp_err.h"
#include <deque>

#if CONFIG_FREERTOS_UNICORE != 1 
#error Arduino idf core must be compiled with CONFIG_FREERTOS_UNICORE=y and CONFIG_ESP_INT_WDT=n
#endif

#else 
#include "esp32csim.h"
#endif

#include <vector>
#include <string>
using std::vector;
using std::string;
#include "ascii2keypress.h"


#include "core1.h" 

// Usable pins
// 0-18 21            (20)
// 38-48 (6-16)     (11)
//pin 01234567890123456789012345678901
//    11111111111111111110001000000000
//    00000011111111110000000000000000

//data 8
//addr 16
//clock sync halt write 
// TODO: investigate GPIO input filter, GPIO output sync 

//GPIO0 bits: TODO rearrange bits so addr is in low bits and avoids needed a shift
// Need 19 pines on gpio0: ADDR(16), clock, casInh, RW

unsigned IRAM_ATTR my_nmi(unsigned x) { return 0; }

IRAM_ATTR inline void delayTicks(int ticks) { 
    uint32_t startTsc = XTHAL_GET_CCOUNT();
    while(XTHAL_GET_CCOUNT() - startTsc < ticks) {}
}

DRAM_ATTR RAM_VOLATILE uint8_t *banks[nrBanks * 4];
DRAM_ATTR uint32_t bankEnable[nrBanks * 4];
DRAM_ATTR RAM_VOLATILE uint8_t atariRam[64 * 1024] = {0x0};
DRAM_ATTR RAM_VOLATILE uint8_t dummyRam[bankSize] = {0x0};
DRAM_ATTR RAM_VOLATILE uint8_t bankD100Write[bankSize] = {0x0};
DRAM_ATTR RAM_VOLATILE uint8_t bankD100Read[bankSize] = {0x0};
//DRAM_ATTR RAM_VOLATILE uint8_t cartROM[] = {
//#include "joust.h"
//};
DRAM_ATTR RAM_VOLATILE uint8_t pbiROM[2 * 1024] = {
#include "pbirom.h"
};
DRAM_ATTR RAM_VOLATILE uint8_t page6Prog[] = {
#include "page6.h"
};
DRAM_ATTR uint8_t diskImg[] = {
#include "disk.h"
};

BUSCTL_VOLATILE DRAM_ATTR uint32_t busMask = extSel_Mask;

IRAM_ATTR void memoryMapInit() { 
    for(int i = 0; i < nrBanks; i++) {
        banks[i | BANKSEL_ROM | BANKSEL_RD] = &dummyRam[0];
        banks[i | BANKSEL_ROM | BANKSEL_WR] = &dummyRam[0];
        banks[i | BANKSEL_RAM | BANKSEL_RD] = &atariRam[64 * 1024 / nrBanks * i];
        banks[i | BANKSEL_RAM | BANKSEL_WR] = &atariRam[64 * 1024 / nrBanks * i];
    };

    static const int d100Bank = (0xd1ff >> bankShift);
    banks[d100Bank | BANKSEL_ROM | BANKSEL_WR ] = &bankD100Write[0]; 
    banks[d100Bank | BANKSEL_ROM | BANKSEL_RD ] = &bankD100Read[0]; 
    banks[d100Bank | BANKSEL_RAM | BANKSEL_WR ] = &bankD100Write[0]; 
    banks[d100Bank | BANKSEL_RAM | BANKSEL_RD ] = &bankD100Read[0]; 
}

DRAM_ATTR int deferredInterrupt = 0, interruptRequested = 0;

IRAM_ATTR void raiseInterrupt() {
    if ((atariRam[PDIMSK] & pdiDeviceNum) == pdiDeviceNum) {
        deferredInterrupt = 0;  
        bankD100Read[0xd1ff & bankOffsetMask] = 0x1;
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, interruptMask);
        interruptRequested = 1;
    } else { 
        deferredInterrupt = 1;
    }
}

IRAM_ATTR void clearInterrupt() { 
    bankD100Read[0xd1ff & bankOffsetMask] = 0x0;
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, interruptMask);
    interruptRequested = 0;
}

IRAM_ATTR void enableBus() { 
    for(int i = 0; i < nrBanks; i++) { 
        bankEnable[i | BANKSEL_ROM | BANKSEL_RD] = 0;
        bankEnable[i | BANKSEL_RAM | BANKSEL_RD] = dataMask | extSel_Mask;
    }

    // enable "ROM" reads and writes to 0xd100-0xd1ff 
    static const int d100Bank = (0xd1ff >> bankShift);
    bankEnable[d100Bank | BANKSEL_ROM | BANKSEL_RD] = dataMask | extSel_Mask;
    delayTicks(240 * 100);
}

IRAM_ATTR void enableSingleBank(int i) {
    bankEnable[i | BANKSEL_ROM | BANKSEL_RD] = 0;
    bankEnable[i | BANKSEL_RAM | BANKSEL_RD] = dataMask | extSel_Mask;
}

IRAM_ATTR void disableSingleBank(int i) {
    bankEnable[i | BANKSEL_ROM | BANKSEL_RD] = 0;
    bankEnable[i | BANKSEL_RAM | BANKSEL_RD] = 0;
}

IRAM_ATTR void disableBus() {
    // TODO: Need to disable interrupt bit in PDIMSK before detaching the bus
    // TODO need to use a new global busEnable mask to disable the bus rather
    // than going through all 256 page table entries 
    delayTicks(240 * 100);    
    for(int i = 0; i < nrBanks; i++) { 
        bankEnable[i | BANKSEL_ROM | BANKSEL_RD] = 0;
        bankEnable[i | BANKSEL_RAM | BANKSEL_RD] = 0;
    }
}

std::string vsfmt(const char *format, va_list args);
std::string sfmt(const char *format, ...);
class LineBuffer {
public:
        char line[1024];
        char len = 0;
        int add(char c, std::function<void(const char *)> f = NULL);
        void add(const char *b, int n, std::function<void(const char *)> f);
        void add(const uint8_t *b, int n, std::function<void(const char *)> f);
};


const esp_partition_t *partition;
#define LOCAL_LFS
#ifdef LOCAL_LFS
#include "lfs.h"
// variables used by the filesystem
lfs_t lfs;
lfs_file_t file, lfs_diskImg;

size_t partition_size = 0x20000;
const int lfsp_block_sz = 4096;
extern struct lfs_config cfg;

int lfsp_init() { 
    partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    cfg.block_size = partition->erase_size;
    cfg.block_count = partition->size / cfg.block_size;
    //printf("partition find returned %p, part->erase_size %d\n", partition, partition->erase_size);
    return 0;
}
int lfsp_read_block(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, void *buffer, lfs_size_t size) { 
    //printf("read blk %d off %d size %d\n", block, off, size);                
    return esp_partition_read(partition, block * lfsp_block_sz + off, buffer, size);
}
int lfsp_prog_block(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, const void *buffer, lfs_size_t size) { 
    //printf("prog blk %d off %d size %d\n", block, off, size);                
    return esp_partition_write(partition, block * lfsp_block_sz + off, buffer, size);
}
int lfsp_erase_block(const struct lfs_config *c, lfs_block_t block) { 
    //printf("erase blk %d\n", block);
    return esp_partition_erase_range(partition, lfsp_block_sz * block, lfsp_block_sz);
}

int lsfp_sync(const struct lfs_config *c) { return 0; }

struct lfs_config cfg = {
    // block device operations
    .read  = lfsp_read_block,
    .prog  = lfsp_prog_block,
    .erase = lfsp_erase_block,
    .sync  = lsfp_sync,

    // block device configuration
    .read_size = 16,
    .prog_size = 16,
    .block_size = 4096,
    .block_count = 0x20000 / 4096,
    .block_cycles = 500,
    .cache_size = 16,
    .lookahead_size = 16,
};

int lfs_updateTestFile() { 
      // read current count
    uint32_t boot_count = 0;
    lfs_file_open(&lfs, &file, "boot_count", LFS_O_RDWR | LFS_O_CREAT);
    lfs_file_read(&lfs, &file, &boot_count, sizeof(boot_count));

    // update boot count
    boot_count += 1;
    lfs_file_rewind(&lfs, &file);
    lfs_file_write(&lfs, &file, &boot_count, sizeof(boot_count));
    lfs_file_close(&lfs, &file);

    return boot_count;
}
#endif

DRAM_ATTR static const int psram_sz = 6 * 1024 * 1024;
DRAM_ATTR uint32_t *psram;
DRAM_ATTR uint32_t *psram_end;

DRAM_ATTR static const int testFreq = 1.8 * 1000000;//1000000;
DRAM_ATTR static const int lateThresholdTicks = 180 * 2 * 1000000 / testFreq;
static const uint32_t halfCycleTicks = 240 * 1000000 / testFreq / 2;

inline IRAM_ATTR void busyWaitTicks(uint32_t cycles) { 
    uint32_t tsc = XTHAL_GET_CCOUNT();
    while(XTHAL_GET_CCOUNT() - tsc < cycles) {};
}

inline IRAM_ATTR void busywait(float sec) {
    uint32_t tsc = XTHAL_GET_CCOUNT();
    busyWaitTicks(sec * 240 * 1000000);
}

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

struct AtariIOCB { 
    uint8_t ICHID,  // handler 
            ICDNO,  // Device number
            ICCOM,  // Command byte 
            ICSTA,  // Status returned
            ICBAL,  // Buffer address (points to 0x9b-terminated string for open command)
            ICBAH,
            ICPTL,  // Address of driver put routine
            ICPTH,
            ICBLL,  // Buffer length 
            ICBLH,
            ICAX1,
            ICAX2,
            ICAX3,
            ICAX4,
            ICAX5,
            ICAX6;
};

DRAM_ATTR const struct AtariDefStruct {
    int IOCB0 = 0x340;
    int ZIOCB = 0x20;
    int NUMIOCB = 0x8;
    int IOCB_CMD_CLOSE = 0xc;
    int IOCB_CMD_OPEN = 0x3;
    int IOCB_OPEN_READ = 0x4;
    int IOCB_OPEN_WRITE = 0x8;
    int NEWPORT = 0x31ff;
} AtariDef;

DRAM_ATTR Hist2 profilers[numProfilers];
DRAM_ATTR int ramReads = 0, ramWrites = 0;

DRAM_ATTR const char *defaultProgram = 
        "10 A=USR(1546) \233"
        //"11 PRINT A; \233"
        //"12 PRINT \" ->\"; \233"
        //"14 GOTO 10 \233"
        "15 OPEN #1,4,0,\"J2:\" \233"
        "20 GET #1,A  \233"
        //"30 PRINT \"   \"; \233"
        //"35 PRINT A  \233"
        "38 CLOSE #1  \233"
        //"40 GOTO 10 \233"
        "41 OPEN #1,8,0,\"J\" \233"
        "42 PUT #1,A + 1 \233"
        "43 CLOSE #1 \233"
        "51 PRINT \" -> \"; \233"
        "52 PRINT COUNT; \233"
        "53 COUNT = COUNT + 1 \233"
        //"54 OPEN #1,4,0,\"D2:DUP.SYS\" \233"
        "54 OPEN #1,4,0,\"D2:X32Z.DOS\" \233"
        "55 POINT #1,SEC,BYT \233"
        "56 GET #1,A \233"
        "57 CLOSE #1 \233"
        "58 SEC = SEC + 1 \233"
        "59 IF SEC > 40 THEN SEC = 0 \233"
        "70 GOTO 10 \233"
        "RUN\233"
        ;


DRAM_ATTR vector<uint8_t> simulatedKeypressQueue;
DRAM_ATTR int simulatedKeysAvailable = 0;
IRAM_ATTR void addSimKeypress(char c) {
    if (c == '\n') c = '\233';
    simulatedKeypressQueue.push_back(c);
    simulatedKeysAvailable = 1;
}
IRAM_ATTR void addSimKeypress(const string &s) { 
    for(auto a : s) addSimKeypress(a);
}

// CORE0 loop options 
#ifndef FAKE_CLOCK
#define ENABLE_SIO
#define SIM_KEYPRESS
//#define SIM_KEYPRESS_FILE
#endif
struct AtariIO {
    uint8_t buf[2048];
    int ptr = 0;
    int len = 0;
    AtariIO() { 
        strcpy((char *)buf, defaultProgram); 
        len = strlen((char *)buf);
    }
#ifdef SIM_KEYPRESS_FILE

    string filename;
    inline IRAM_ATTR void open(const string &f) { 
        filename = f;
        if (filename == "J:UNMAP") {
            disableSingleBank(0x8000 >> bankShift);
        }
        if (filename == "J:REMAP") {
            enableSingleBank(0x8000 >> bankShift);
        }
#else 
    inline IRAM_ATTR void open() { 
#endif
        ptr = 0; 
    }

    inline IRAM_ATTR int get() { 
        if (ptr >= len) return -1;
        return buf[ptr++];
    }
    inline IRAM_ATTR int put(uint8_t c) { 
        if (ptr >= sizeof(buf)) return -1;
        buf[ptr++] = c;
        len = ptr;
#ifdef SIM_KEYPRESS_FILE
        if (filename == "J:KEYS") addSimKeypress(c);
#endif
        return 1;
    }
};
DRAM_ATTR AtariIO fakeFile; 

struct AtariDCB { 
   uint8_t 
    DDEVIC,
    DUNIT,
    DCOMND,
    DSTATS,
    DBUFLO,
    DBUFHI,
    DTIMLO,
    DUNUSED,
    DBYTLO,
    DBYTHI,
    DAUX1,
    DAUX2;
};

DRAM_ATTR struct { 
    AtariDCB *dcb = (AtariDCB *)&atariRam[0x300];
    AtariIOCB *ziocb = (AtariIOCB *)&atariRam[0x20];
    AtariIOCB *iocb0 = (AtariIOCB *)&atariRam[0x320];
} atariMem;

struct PbiIocb {
    uint8_t req;
    uint8_t cmd;
    uint8_t a;
    uint8_t x;

    uint8_t y;
    uint8_t carry;
    uint8_t critic;
    uint8_t psp;

    uint8_t nmien;
    uint8_t rtclok1;
    uint8_t rtclok2;
    uint8_t rtclok3;

    uint8_t loc004d;
    uint8_t loc004e;
    uint8_t loc004f;
    uint8_t romAddrSignatureCheck;
};

//#define STRUCT_LOG
#ifdef STRUCT_LOG 
template<class T> 
struct StructLog { 
    int maxSize;
    uint32_t lastTsc;
    StructLog(int maxS = 32) : maxSize(maxS) {}
    std::deque<std::pair<uint32_t,T>> log;
    inline void IRAM_ATTR add(const T &t) {
        uint32_t tsc = XTHAL_GET_CCOUNT(); 
        log.push_back(std::pair<uint32_t,T>(tsc - lastTsc, t));
        lastTsc = tsc;
        if (log.size() > maxSize) log.pop_front();
    }
    static inline IRAM_ATTR void  printEntry(const T&);
    inline void IRAM_ATTR print() { 
        for(auto a : log) {
            printf("%-10d : ", a.first);
            printEntry(a.second);
        } 
    }
};
template <class T> inline IRAM_ATTR void StructLog<T>::printEntry(const T &a) {
    for(int i = 0; i < sizeof(a); i++) printf("%02x ", ((uint8_t *)&a)[i]);
    printf("\n");
}
template <> inline void StructLog<string>::printEntry(const string &a) { 
    printf("%s\n", a.c_str()); 
}
#else //#ifdef STRUCT_LOG 
template<class T> 
struct StructLog {
    StructLog(int maxS = 32) {}
    inline void IRAM_ATTR add(const T &t) {} 
    static inline IRAM_ATTR void  printEntry(const T&) {}
    inline void IRAM_ATTR print() {}
};
#endif

DRAM_ATTR struct { 
    StructLog<AtariDCB> dcb; 
    StructLog<AtariIOCB> iocb; 
    StructLog<PbiIocb> pbi = StructLog<PbiIocb>(100);
    StructLog<AtariIOCB> ziocb; 
    StructLog<string> opens;
    void print() {
        printf("PBI log:\n"); pbi.print();
        printf("DCB log:\n"); dcb.print();
        printf("IOCB log:\n"); iocb.print();
        printf("ZIOCB log:\n"); ziocb.print();
        printf("opened files log:\n"); opens.print();
    }
} structLogs;

// https://www.atarimax.com/jindroush.atari.org/afmtatr.html
struct AtrImageHeader {
    uint16_t magic; // 0x0296;
    uint16_t pars;  // disk image size divided by 0x10
    uint16_t sectorSize; // usually 0x80 or 0x100
    uint8_t parsHigh; // high byte of larger wPars size (added in rev3.00)
    uint32_t crc;       
    uint32_t unused;
    uint8_t flags;
};

struct DiskImage {
    string hostFilename;
    union DiskImageRawData { 
        uint8_t data[1]; 
        AtrImageHeader header;
    } *image;
};

DRAM_ATTR DiskImage atariDisks[8] =  {
    {"none", (DiskImage::DiskImageRawData *)diskImg}, 
    {"none", (DiskImage::DiskImageRawData *)diskImg}, 
};

DRAM_ATTR int diskReadCount = 0, pbiInterruptCount = 0, memWriteErrors = 0;
DRAM_ATTR string exitReason = "";
DRAM_ATTR int elapsedSec = 0;

#define EVERYN_TICKS(ticks) \
    DRAM_ATTR static uint32_t lastTsc ## __LINE__ = XTHAL_GET_CCOUNT(); \
    const uint32_t tsc ## __LINE__ = XTHAL_GET_CCOUNT(); \
    bool doLoop ## __LINE__ = false; \
    if(tsc ## __LINE__ - lastTsc ## __LINE__ > ticks) {\
        lastTsc ## __LINE__ = tsc ## __LINE__; \
        doLoop ## __LINE__ = true; \
    } \
    if (doLoop ## __LINE__)

void IRAM_ATTR handlePbiRequest(PbiIocb *pbiRequest) { 
    // TMP: put the shortest, quickest interrupt service possible
    // here 
    if(1 && pbiRequest->cmd == 8) { 
        pbiRequest->carry = interruptRequested;
        clearInterrupt();
        pbiRequest->req = 0;
        pbiInterruptCount++;
        return;
    }
#ifdef BUS_DETACH
    // Disable PBI memory device 
    disableBus();
    structLogs.pbi.add(*pbiRequest);
    if (1) { 
        DRAM_ATTR static int lastPrint = -999;
        if (elapsedSec - lastPrint >= 2) { 
            enableCore0WDT();
            portENABLE_INTERRUPTS();
            lastPrint = elapsedSec;
            printf("time %02d:%02d:%02d iocount: %8d  irq: %d pin 48 %d\n", 
                elapsedSec/3600, (elapsedSec/60)%60, elapsedSec%60, diskReadCount, pbiInterruptCount, digitalRead(48));
            fflush(stdout);
            portDISABLE_INTERRUPTS();
            disableCore0WDT();
        }
    }
    if (0 && elapsedSec > 8) { 
        enableCore0WDT();
        portENABLE_INTERRUPTS();
        printf("IO request: ");
        StructLog<PbiIocb>::printEntry(*pbiRequest);
        fflush(stdout);
        portDISABLE_INTERRUPTS();
        disableCore0WDT();
    }
    while(Serial.available()) { 
        enableCore0WDT();
        portENABLE_INTERRUPTS();
        static LineBuffer lb;
        lb.add(Serial.read(), [](const char *line) {
            char c; 
            if (sscanf(line, "key %c", &c) == 1) {
                printf("keypress key '%c'\n", c);
                addSimKeypress(c);
            }
        });
        fflush(stdout);
        portDISABLE_INTERRUPTS();
        disableCore0WDT();
    }
#endif // #ifdef BUS_DETACH
    AtariIOCB *iocb = (AtariIOCB *)&atariRam[AtariDef.IOCB0 + pbiRequest->x]; // todo validate x bounds
    //pbiRequest->y = 1; // assume success
    //pbiRequest->carry = 0; // assume fail 
    if (pbiRequest->cmd == 1) { // open
        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 0; // assume fail 
        uint16_t addr = ((uint16_t )atariMem.ziocb->ICBAH) << 8 | atariMem.ziocb->ICBAL;
#ifdef SIM_KEYPRESS_FILE
        string filename;
        for(int i = 0; i < 32; i++) { 
            uint8_t ch = atariRam[addr + i];
            if (ch == 155) break;
            filename += ch;    
        } 
        structLogs.opens.add(filename);
        //structLogs.ziocb.add(*atariMem.ziocb);
        //
        fakeFile.open(filename);
#else
        fakeFile.open();
        structLogs.iocb.add(*iocb);
#endif
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 2) { // close
        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 3) { // get
        pbiRequest->y = 1; // assume success
        int c = fakeFile.get();
        if (c < 0) 
            pbiRequest->y = 136;
        else
            pbiRequest->a = c; 
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 4) { // put
        pbiRequest->y = 1; // assume success
        if (fakeFile.put(pbiRequest->a) < 0)
            pbiRequest->y = 136;
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 5) { // status 
        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 0; // assume fail 
    } else if (pbiRequest->cmd == 6) { // special 
        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 0; // assume fail 
    } else if (pbiRequest->cmd == 7) { // low level io, see DCB
#ifdef ENABLE_SIO
        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 0; // assume fail 
        AtariDCB *dcb = atariMem.dcb;
        uint16_t addr = (((uint16_t)dcb->DBUFHI) << 8) | dcb->DBUFLO;
        int sector = (((uint16_t)dcb->DAUX2) << 8) | dcb->DAUX1;
        structLogs.dcb.add(*dcb);
#ifdef BUS_DETACH
        if (0) { 
            enableCore0WDT();
            portENABLE_INTERRUPTS();
            printf("DCB: ");
            StructLog<AtariDCB>::printEntry(*dcb);
            fflush(stdout);
            portDISABLE_INTERRUPTS();
            disableCore0WDT();
        }
#endif
        if (dcb->DDEVIC == 0x31 && dcb->DUNIT >= 1 && dcb->DUNIT < sizeof(atariDisks)/sizeof(atariDisks[0]) + 1) {  // Device D1:
            DiskImage::DiskImageRawData *disk = atariDisks[dcb->DUNIT - 1].image; 
            if (disk != NULL) { 
                int sectorSize = disk->header.sectorSize;
                if (dcb->DCOMND == 0x53) { // SIO status command
                    // drive status https://www.atarimax.com/jindroush.atari.org/asio.html
                    atariRam[addr+0] = (sectorSize == 0x100) ? 0x10 : 0x00; // bit 0 = frame err, 1 = cksum err, wr err, wr prot, motor on, sect size, unused, med density  
                    atariRam[addr+1] = 0xff; // inverted bits: busy, DRQ, data lost, crc err, record not found, head loaded, write pro, not ready 
                    atariRam[addr+2] = 0xff; // timeout for format 
                    atariRam[addr+3] = 0xff; // copy of wd
                    pbiRequest->carry = 1;
                }
                int sectorOffset = 16 + (sector - 1) * sectorSize;
                if (dcb->DCOMND == 0x52) {  // READ sector
                    if (dcb->DUNIT == 1) {
                        for(int n = 0; n < sectorSize; n++) 
                            atariRam[addr + n] = disk->data[sectorOffset + n];
                        //memcpy(&atariRam[addr], &disk->data[sectorOffset], sectorSize);
                        pbiRequest->carry = 1;
                    } else if (dcb->DUNIT == 2) { 
                        enableCore0WDT();
                        portENABLE_INTERRUPTS();
                        lfs_file_seek(&lfs, &lfs_diskImg, sectorOffset, LFS_SEEK_SET);
                        size_t r = lfs_file_read(&lfs, &lfs_diskImg, &atariRam[addr], sectorSize);                                    
                        //printf("lfs_file_read() returned %d\n", r);
                        fflush(stdout);
                        portDISABLE_INTERRUPTS();
                        disableCore0WDT();
                        pbiRequest->carry = 1;
                    }
                }
                if (dcb->DCOMND == 0x50) {  // WRITE sector
                    if (dcb->DUNIT == 1) {
                        for(int n = 0; n < sectorSize; n++) 
                            disk->data[sectorOffset + n] = atariRam[addr + n];
                        //memcpy(&disk->data[sectorOffset], &atariRam[addr], sectorSize);
                        pbiRequest->carry = 1;
                    } else if (dcb->DUNIT == 2) { 
                        enableCore0WDT();
                        portENABLE_INTERRUPTS();
                        lfs_file_seek(&lfs, &lfs_diskImg, sectorOffset, LFS_SEEK_SET);
                        size_t r = lfs_file_write(&lfs, &lfs_diskImg, &atariRam[addr], sectorSize);                                    
                        //printf("lfs_file_write() returned %d\n", r);
                        fflush(stdout);
                        portDISABLE_INTERRUPTS();
                        disableCore0WDT();
                        pbiRequest->carry = 1;
                    }
                }
            }
        }
#endif // ENABLE_SIO 
    } else if (pbiRequest->cmd == 8) { // IRQ
        pbiRequest->carry = interruptRequested;  
        clearInterrupt();
        if (pbiRequest->carry == 0) { 
            enableCore0WDT();
            portENABLE_INTERRUPTS();
            printf("IRQ, req=%d: ", pbiRequest->carry);
            StructLog<PbiIocb>::printEntry(*pbiRequest);
            fflush(stdout);
            portDISABLE_INTERRUPTS();
            disableCore0WDT();
        }

        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 1;
        //REG_WRITE(GPIO_OUT1_W1TC_REG, interruptMask);
        atariRam[712]++; // TMP: increment border color as visual indicator 
        pbiInterruptCount++;
    } else if (pbiRequest->cmd == 9) { // REMAP
        // called after each command to re-enable the bus, we leave
        // pbiRequest->{a,x,y,carry} containing the previous command results
        #ifdef BUS_DETACH
        atariRam[0x0012] = pbiRequest->rtclok1;
        atariRam[0x0013] = pbiRequest->rtclok2;
        atariRam[0x0014] = pbiRequest->rtclok3;
        atariRam[0x004d] = pbiRequest->loc004d;
        atariRam[0x004e] = pbiRequest->loc004e;
        atariRam[0x004f] = pbiRequest->loc004f;
        enableBus();
        #endif
    } 

    // TODO:  enableSingleBank(0xd800>>bankShift), then return and let
    // the 6502 copy out critical memory locations to struct, 
    // then 6502 make another pbiReq->cmd == remap to tidy up
    // changed ram locations and remap esp32 ram
    //
    // alternatively, if its only the clock locations that are changed,
    // maybe just fake them and don't bother with a two-stage completion process  

    #ifdef BUS_DETACH
    for(int i = 0; i < 2048 / bankSize; i++) { 
        enableSingleBank((0xd800 >> bankShift) + i);
    }
    #endif
    pbiRequest->req = 0;
}

DRAM_ATTR static uint8_t dummyMem[0x400];

void IRAM_ATTR core0Loop() { 

    enableBus();
    uint32_t *psramPtr = psram;
#ifdef RAM_TEST
    // disable PBI ROM by corrupting it 
    pbiROM[0x03] = 0xff;
#endif

    uint32_t lastBmon = 0;
    int bmonCaptureDepth = 0;
    struct BmonTrigger { 
        uint32_t mask;
        uint32_t value;
        int depth;
        int preroll;
        int count;
        int skip;
    };

    const int prerollBufferSize = 64; // must be power of 2
    uint32_t prerollBuffer[prerollBufferSize]; 
    uint32_t prerollIndex = 0;

    const vector<BmonTrigger> bmonTriggers = {
        { 
            .mask = (readWriteMask | addrMask) << bmonR0Shift, 
            .value = (readWriteMask | (0x0611 << addrShift)) << bmonR0Shift,
            .depth = 40,
            .preroll = 4,
            .count = 1000000,
            .skip = 0 // TODO - doesn't work? 
        },
    };

    while(1) {
        uint32_t stsc = XTHAL_GET_CCOUNT();
        stsc = XTHAL_GET_CCOUNT();
        uint32_t bmon = 0;
        while(XTHAL_GET_CCOUNT() - stsc < 240 * 1000 * 50) {  
            while(
                //XTHAL_GET_CCOUNT() - stsc < 240 * 1000 * 50 && 
                (bmon = REG_READ(SYSTEM_CORE_1_CONTROL_1_REG)) == lastBmon) {}

            bmon = bmon & 0x2fffffff;    
            uint32_t r0 = bmon >> bmonR0Shift;
            if (bmon == lastBmon || (r0 & refreshMask) == 0) 
                continue;

            lastBmon = bmon;

            if (bmonCaptureDepth > 0) {
                bmonCaptureDepth--;
                *psramPtr = bmon;
                psramPtr++;
                if (psramPtr == psram_end) 
                    psramPtr = psram; 
            } else { 
                for(auto t : bmonTriggers) {
                    if (t.count > 0 && (bmon & t.mask) == t.value) {
                        if (t.skip > 0) { 
                            t.skip--;
                        } else {
                            bmonCaptureDepth = t.depth;
                            t.count--;

                            for(int i = min(prerollBufferSize, t.preroll) - 1; i >= 0; i--) { 
                                // Compute backIdx as prerollIndex - i;
                                int backIdx = (prerollIndex + (prerollBufferSize - i)) & (prerollBufferSize - 1);
                                *psramPtr = prerollBuffer[backIdx];
                                psramPtr++;
                                if (psramPtr == psram_end) 
                                    psramPtr = psram; 
                            }

                            bmon |= 0x80000000; 
                            *psramPtr = bmon;
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
            prerollBuffer[prerollIndex] = bmon;
            prerollIndex = (prerollIndex + 1) & (prerollBufferSize - 1); 
            if ((r0 & readWriteMask) == 0) {
                uint32_t lastWrite = (r0 & addrMask) >> addrShift;
                if (lastWrite == 0x0600) break;
                if (lastWrite == 0xd830) break;
                if (lastWrite == 0xd820) break;
                if (lastWrite == PDIMSK) break;
            }    
        }


        if (deferredInterrupt && (atariRam[PDIMSK] & pdiDeviceNum) == pdiDeviceNum)
            raiseInterrupt();

        if (0 && elapsedSec > 15) { // XXINT
            static uint32_t ltsc = 0;
            if (XTHAL_GET_CCOUNT() - ltsc > 240 * 1000 * 100) { 
                ltsc = XTHAL_GET_CCOUNT();
                raiseInterrupt();
            }
        }

        if (1) { // XXMEMTEST
            if (atariRam[1536] != 0 &&
                atariRam[1538] == 0xde && 
                atariRam[1539] == 0xad &&
                atariRam[1540] == 0xbe &&
                atariRam[1541] == 0xef) {
                    int cmd = atariRam[1536];
                    if (cmd == 1) { 
                        // remap 
                        for(int mem = 0x8000; mem < 0x8400; mem += 0x100) { 
                            banks[nrBanks * 1 + ((mem + 0x400) >> bankShift)] = &atariRam[mem];
                            banks[nrBanks * 3 + ((mem + 0x400) >> bankShift)] = &atariRam[mem];
                        }
                    }
                    if (0 && cmd == 2) {  
                        for(int mem = 0x8000; mem < 0x8400; mem += 0x100) { 
                            for(int i = 0; i < 256; i++) {
                                if (atariRam[mem + i] != i) memWriteErrors++;
                            }
                        }
                    }
                    atariRam[1536] = 0;
                    diskReadCount++; 
            }
        }

#if #defined(FAKE_CLOCK) || #defined (RAM_TEST)
        if (1 && elapsedSec > 10) { //XXFAKEIO
            // Stuff some fake PBI commands to exercise code in the core0 loop during timing tests 
            static uint32_t lastTsc;
            if (XTHAL_GET_CCOUNT() - lastTsc > 240 * 1000) {
                lastTsc = XTHAL_GET_CCOUNT();
                volatile PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x20];
                static int step = 0;
                if (step == 1) { 
                    // stuff a fake CIO put request
                    #ifdef SIM_KEYPRESS_FILE
                    fakeFile.filename = "J:KEYS";
                    #endif 
                    pbiRequest->cmd = 4; // put 
                    pbiRequest->a = ' ';
                    pbiRequest->req = 1;
                } else if (step == 2) { 
                    // stuff a fake SIO sector read request 
                    volatile AtariDCB *dcb = atariMem.dcb;
                    dcb->DBUFHI = 0x40;
                    dcb->DBUFLO = 0x00;
                    dcb->DDEVIC = 0x31; 
                    dcb->DUNIT = 1;
                    dcb->DAUX1++; 
                    dcb->DAUX2 = 0;
                    dcb->DCOMND = 0x52;
                    pbiRequest->cmd = 7; // read a sector 
                    pbiRequest->req = 1;
                } else if (step == 2) { 
                    
                }
                step = (step + 1) % 3;
            }
        }
#endif 

#ifdef SIM_KEYPRESS
        { // TODO:  EVERYN_TICKS macro broken, needs its own scope. 
            static const int keyMs = 150;
            EVERYN_TICKS(240 * 1000 * keyMs) { 
                if (simulatedKeysAvailable && simulatedKeypressQueue.size() > 0) { 
                    uint8_t c = simulatedKeypressQueue[0];
                    simulatedKeypressQueue.erase(simulatedKeypressQueue.begin());
                    if (c != 255) 
                        atariRam[764] = ascii2keypress[c];
                } else { 
                    simulatedKeysAvailable = 0;
                }
            }
        }
#endif
        if (1) {  
            //volatile
            PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x20];
            if (pbiRequest[0].req != 0) { 
                #ifndef RAM_TEST
                diskReadCount++;
                #endif
                handlePbiRequest(&pbiRequest[0]); 
            }
            if (pbiRequest[1].req != 0) { 
                handlePbiRequest(&pbiRequest[1]);
            }
        }
        EVERYN_TICKS(240 * 1000000) { // XXSECOND
            elapsedSec++;
     
            //if (elapsedSec == 30) raiseInterrupt();

            if (elapsedSec == 8 && diskReadCount == 0) {
                memcpy(&atariRam[0x0600], page6Prog, sizeof(page6Prog));
                addSimKeypress("A=USR(1546)\233");
            }

            if (elapsedSec == 8 && diskReadCount > 0) {
                memcpy(&atariRam[0x0600], page6Prog, sizeof(page6Prog));
                addSimKeypress("E.\"J\233");
                //addSimKeypress("    \233DOS\233     \233DIR D2:\233");
            }

            #ifndef FAKE_CLOCK
            if (1) { 
                DRAM_ATTR static int lastReads = 0;
                DRAM_ATTR static int secondsWithoutRead = 0;
                if (1) { 
                    if (diskReadCount == lastReads) { 
                        secondsWithoutRead++;
                    } else { 
                        secondsWithoutRead = 0;
                    }
                } else { 
                    if (atariRam[1537] == 0) { 
                        secondsWithoutRead++;
                    }
                    atariRam[1537] = 0;
                }

                lastReads = diskReadCount;
                if (secondsWithoutRead == 5) { 
                    for(int i = 0; i < sizeof(atariRam); i++) { 
                            psram[i] = 0;
                    }
                }
                if (secondsWithoutRead == 30) { 
                    exitReason = "-1 Timeout with no IO requests";
                    break;
                }
            }
            #endif

            if (elapsedSec == 1) { 
               for(int i = 0; i < numProfilers; i++) profilers[i].clear();
            }
            if(elapsedSec > opt.histRunSec && opt.histRunSec > 0) {
                exitReason = "0 Specified run time reached";   
                break;
            }
            if(atariRam[754] == 23 || atariRam[764] == 23) {
                exitReason = "1 Z key pressed";
                break;
            }
        }
    }
}

void threadFunc(void *) { 
    printf("CORE0: threadFunc() start\n");

#ifdef BUS_DETACH
    printf("BUS_DETACH is set\n");
#else
    printf("BUS_DETACH is NOT set\n");
#endif
    printf("opt.fakeClock %d opt.histRunSec %.2f\n", opt.fakeClock, opt.histRunSec);
    printf("GIT: " GIT_VERSION " \n");

    XT_INTEXC_HOOK oldnmi = _xt_intexc_hooks[XCHAL_NMILEVEL];
    uint32_t oldint;

    portDISABLE_INTERRUPTS();
    disableCore0WDT();
    _xt_intexc_hooks[XCHAL_NMILEVEL] = my_nmi; 
    //__asm__ __volatile__("rsil %0, 1" : "=r"(oldint) : );

    core0Loop();
    disableBus();

    busywait(.001);
    REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
    busywait(.001);
    
    enableCore0WDT();
    portENABLE_INTERRUPTS();
    _xt_intexc_hooks[XCHAL_NMILEVEL] = oldnmi;
    //__asm__("wsr %0,PS" : : "r"(oldint));

    uint64_t totalEvents = 0;
    for(int i = 0; i < profilers[0].maxBucket; i++)
        totalEvents += profilers[0].buckets[i];
    printf("Total samples %lld implies %.2f sec sampling. Total reads %d\n",
        totalEvents, 1.0 * totalEvents / 1.8 / 1000000, ramReads);

    if (opt.histogram) {
        vector<string> v; 
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
            string s = sfmt("% 4d ", i);
            for(int c = 0; c < numProfilers; c++) {
                s += sfmt("% 12d ", profilers[c].buckets[i]);
            }
            s += " HIST";
            v.push_back(s);
        }

        for (int c = 0; c < numProfilers; c++) {
            first = last = 0; 
            for(int i = 1; i < profilers[c].maxBucket; i++) { 
                if (profilers[c].buckets[i] > 0) last = i;
            }
            for(int i = profilers[c].maxBucket - 1; i > 0 ;i--) { 
                if (profilers[c].buckets[i] > 0) first = i;
            }
            yield();
            v.push_back(sfmt("channel %d: range %3d -%3d, jitter %3d    HIST", c, first, last, last - first));
        }
        uint64_t totalEvents = 0;
        for(int i = 0; i < profilers[0].maxBucket; i++)
            totalEvents += profilers[0].buckets[i];
        v.push_back(sfmt("Total samples %lld implies %.2f sec sampling\n", totalEvents, 1.0 * totalEvents / 1.8 / 1000000));

        for(auto s : v) 
            printf("%s\n", s.c_str());
    }
    
    printf("DUMP %.2f\n", millis() / 1000.0);
    
    if (opt.tcpSendPsram) { 
        printf("TCP SEND %.2f\n", millis() / 1000.0);
        //j.begin();
        //wdtReset();
        yield();
        //disableCore0WDT();
        //disableLoopWDT();
        while(!sendPsramTcp((char *)psram, psram_sz)) delay(1000);
    }
    if (opt.dumpPsram) { 
        for(uint32_t *p = psram; p < psram + min(opt.dumpPsram, psram_end - psram); p++) {
            //printf("P %08X\n",*p);
            //if ((*p & copyResetMask) && !(*p &casInh_Mask))
            //if ((*p & copyResetMask) != 0)
            //s += sfmt("%08x\n", *p);
            if (0) { 
                //if ((*p & copyResetMask) != 0)
                printf("P%08x %08x %04x %02x MPD%d C%d RW%d\n", 
                    (int)(p - psram), *p, (*p & addrMask) >> addrShift,
                    (*p & copyDataMask) >> copyDataShift, 
                    (*p& copyMpdMask) != 0, 
                    (*p & casInh_Mask) != 0,
                    (*p & readWriteMask) != 0);
                //printf("P %08X %08X\n", p - psram, *p);
            }
            if (1) {
                uint32_t r0 = ((*p) >> 8);
                uint16_t addr = r0 >> addrShift;
                char rw = (r0 & readWriteMask) != 0 ? 'R' : 'W';
                uint8_t data = (*p & 0xff);
                printf("P %08x %c %04x %02x\n", *p, rw, addr, data); 
            }
            //if (p > psram + 1000) break;
            //wdtReset();
            //if (((p - psram) % 0x1000) == 0) printf("%08x\n", p - psram);
        }
        yield();
    }
    printf("bank 0xd8 %p, pbi rom is %p, atari ram is %p\n", banks[0xd800 >> bankShift], pbiROM, &atariRam[0xd800]);
    if (banks[0xd800 >> bankShift] == &atariRam[0xd800]) {
        printf("bank 0xd8 set to atari ram\n");
    }
    if (banks[0xd800 >> bankShift] == pbiROM) {
        printf("bank 0xd8 set to PBI ROM\n");
    }
    printf("atariRam[754] = %d\n", atariRam[754]);
    printf("pbiROM[0x100] = %d\n", pbiROM[0x100]);
    printf("atariRam[0xd900] = %d\n", atariRam[0xd900]);
    printf("diskIoCount %d, pbiInterruptCount %d\n", diskReadCount, pbiInterruptCount);
    structLogs.print();
    printf("Page 6: ");
    for(int i = 0x600; i < 0x620; i++) { 
        printf("%02x ", atariRam[i]);
    }
    printf("\n");

#ifndef FAKE_CLOCK
    if (1) {
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        printf("SCREEN 00 memory at SAVMSC(%04x):\n", savmsc);
        printf("SCREEN 01 +----------------------------------------+\n");
        for(int row = 0; row < 24; row++) { 
            printf("SCREEN %02d |", row + 2);
            for(int col = 0; col < 40; col++) { 
                uint16_t addr = savmsc + row * 40 + col;
                uint8_t c = atariRam[addr];
                if (c & 0x80) {
                    printf("\033[7m");
                    c -= 0x80;
                };
                if (c < 64) c += 32;
                else if (c < 96) c -= 64;
                printf("%c\033[0m", c);
            }
            printf("|\n");
        }
        printf("SCREEN 27 +----------------------------------------+\n");
    }
#endif

    printf("\n0xd1ff: %02x\n", atariRam[0xd1ff]);
    printf("0xd820: %02x\n", atariRam[0xd820]);
    //printf("busMask: %08x bus is %s\n", busMask, (busMask & dataMask) == dataMask ? "ENABLED" : "DISABLED");
    
    printf("Minimum free ram: %d bytes\n", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    int memReadErrors = (atariRam[0x609] << 24) + (atariRam[0x608] << 16) + (atariRam[0x607] << 16) + atariRam[0x606];
    printf("SUMMARY %-10.2f/%.0f e%d i%d d%d %s\n", millis()/1000.0, opt.histRunSec, memReadErrors, 
    pbiInterruptCount, diskReadCount, exitReason.c_str());
    printf("DONE %-10.2f READERR %-8d IO %-8d BUILT " __TIME__ " Exit reason: %s\n", 
        millis() / 1000.0, memReadErrors, diskReadCount, exitReason.c_str());
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
static void IRAM_ATTR app_cpu_main();
static void IRAM_ATTR app_cpu_init()
{
    // Reset the reg window. This will shift the A* registers around,
    // so we must do this in a separate ASM block.
    // Otherwise the addresses for the stack pointer and main function will be invalid.
    asm volatile (                                \
        "movi a0, 0\n"                            \
        "wsr  a0, WindowStart\n"                \
        "movi a0, 0\n"                            \
        "wsr  a0, WindowBase\n"                    \
        );
    // init the stack pointer and jump to main function
    asm volatile (                    \
        "l32i a1, %0, 0\n"            \
        "callx4   %1\n"                \
        ::"r"(&app_cpu_stack_ptr),"r"(app_cpu_main));
    REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
}

void startCpu1() {  
    if (REG_GET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN)) {
        printf("APP CPU is already running!\n");
        return;
    }

    if (!app_cpu_stack_ptr) {
        app_cpu_stack_ptr = heap_caps_malloc(1024, MALLOC_CAP_DMA);
    }

    DPORT_REG_WRITE(SYSTEM_CORE_1_CONTROL_1_REG, 0);
    DPORT_REG_WRITE(SYSTEM_CORE_1_CONTROL_0_REG, 0);
    DPORT_REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETING);
    DPORT_REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETING);

    ets_set_appcpu_boot_addr((uint32_t)&app_cpu_init);
    DPORT_REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
}

void setup() {
    for(auto i : pins) pinMode(i, INPUT);
    delay(500);
    Serial.begin(115200);
    printf("setup()\n");

#ifdef LOCAL_LFS
    lfsp_init();
    int err = lfs_mount(&lfs, &cfg);
    printf("lfs_mount() returned %d\n", err);

    // reformat if we can't mount the filesystem
    // this should only happen on the first boot
    if (err) {
        printf("Formatting LFS\n");
        lfs_format(&lfs, &cfg);
        lfs_mount(&lfs, &cfg);
        lfs_file_open(&lfs, &lfs_diskImg, "disk2.atr", LFS_O_RDWR | LFS_O_CREAT);
        lfs_file_write(&lfs, &lfs_diskImg, &diskImg, sizeof(diskImg));
        lfs_file_sync(&lfs, &lfs_diskImg);
        lfs_file_close(&lfs, &lfs_diskImg);
    } 
    printf("LFS mounted: %d total bytes\n", cfg.block_size * cfg.block_count);
    lfs_file_open(&lfs, &lfs_diskImg, "disk2.atr", LFS_O_RDWR | LFS_O_CREAT);
    size_t fsize = lfs_file_size(&lfs, &lfs_diskImg);
    printf("Opened disk2.atr file size %d bytes\n", fsize);
    printf("boot_count: %d\n", lfs_updateTestFile());
    printf("free ram: %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));

#endif
    psram = (uint32_t *) heap_caps_aligned_alloc(64, psram_sz,  MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    psram_end = psram + (psram_sz / sizeof(psram[0]));
    bzero(psram, psram_sz);
    for(auto i : pins) pinMode(i, INPUT);
    while(opt.watchPins) { 
            delay(100);
            printf("PU   %08x %08x\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG));
    }

    if (opt.fakeClock) { // simulate clock signal 
        pinMode(clockPin, OUTPUT);
        digitalWrite(clockPin, 0);
        ledcAttachChannel(clockPin, testFreq, 1, 0);
        ledcWrite(clockPin, 1);

        pinMode(readWritePin, OUTPUT);
        digitalWrite(readWritePin, 0);
        ledcAttachChannel(readWritePin, testFreq / 4, 1, 2);
        ledcWrite(readWritePin, 1);

        pinMode(casInh_pin, OUTPUT);
        digitalWrite(casInh_pin, 1);
        //ledcAttachChannel(casInh_pin, testFreq / 2, 1, 4);
        //ledcWrite(casInh_pin, 1);

        // write 0xd1ff to address pins to simulate worst-case slowest address decode
        for(int bit = 0; bit < 16; bit ++)  
            pinMode(addr0Pin + bit, ((0xd1ff >> bit) & 1) == 1 ? INPUT_PULLUP : INPUT_PULLDOWN);

        //gpio_set_drive_capability((gpio_num_t)clockPin, GPIO_DRIVE_CAP_MAX);
        pinMode(mpdPin, INPUT_PULLDOWN);
        pinMode(refreshPin, INPUT_PULLUP);
        //pinMode(casInh_pin, INPUT_PULLUP);
        pinMode(extSel_Pin, INPUT_PULLUP);
    }

    for(int i = 0; i < 0; i++) { 
        printf("%08x %08x\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    }

    printf("freq %.4fMhz threshold %d halfcycle %d clockMask %08x\n", 
        testFreq / 1000000.0, lateThresholdTicks, halfCycleTicks, clockMask);

    startCpu1();
    busywait(.001);
    //threadFunc(NULL);
    xTaskCreatePinnedToCore(threadFunc, "th", 4 * 1024, NULL, 0, NULL, 0);
    while(1) { yield(); delay(1000); };
}
        
void loop() {
    while(1) { yield(); delay(1); }
}

static void IRAM_ATTR app_cpu_main() {
    uint32_t oldint;
    XT_INTEXC_HOOK oldnmi = _xt_intexc_hooks[XCHAL_NMILEVEL];
    _xt_intexc_hooks[XCHAL_NMILEVEL] = my_nmi;  // saves 5 cycles, could save more 
    __asm__ __volatile__("rsil %0, 15" : "=r"(oldint) : : );
    iloop_pbi();
    while(1) {}
}

#ifdef CSIM
class SketchCsim : public Csim_Module {
    public:
    void setup() { HTTPClient::csim_onPOST("http://.*/log", 
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

// need PBI lines casInh_, WRT, phi2, ADDR0-15, DATA0-7, EXTSEL

// NOTES:
// 8-pin i2c io expander: https://media.digikey.com/pdf/Data%20Sheets/NXP%20PDFs/PCF8574(A).pdf
// TODO: verify polarity of RW, MPD, casInh, etc 


std::string vsfmt(const char *format, va_list args) {
        va_list args2;
        va_copy(args2, args);
        char buf[128]; // don't understand why stack variable+copy is faster
        string rval;

        int n = vsnprintf(buf, sizeof(buf), format, args);
        if (n > sizeof(buf) - 1) {
                rval.resize(n + 2, ' ');
                vsnprintf((char *)rval.data(), rval.size(), format, args2);
                //printf("n %d size %d strlen %d\n", n, (int)rval.size(), (int)strlen(rval.c_str()));
                rval.resize(n);
        } else { 
                rval = buf;
        }
        va_end(args2);
        return rval;
}

std::string sfmt(const char *format, ...) { 
    va_list args;
    va_start(args, format);
        string rval = vsfmt(format, args);
        va_end(args);
        return rval;
}

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

#if 0 
CONFIG_SOC_WDT_SUPPORTED=n
CONFIG_BOOTLOADER_WDT_ENABLE=n
CONFIG_ESP_INT_WDT=n
CONFIG_ESP_TASK_WDT_EN=n
CONFIG_ESP_TASK_WDT_INIT=n
CONFIG_ESP_TASK_WDT_PANIC=n
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n
CONFIG_INT_WDT=n
CONFIG_INT_WDT_CHECK_CPU1=n
CONFIG_INT_WDT_CHECK_CPU0=n
CONFIG_TASK_WDT=n
CONFIG_ESP_TASK_WDT=n
CONFIG_TASK_WDT_CHECK_IDLE_TASK_CPU0=n
CONFIG_TASK_WDT_CHECK_IDLE_TASK_CPU1=n

CONFIG_SOC_XT_WDT_SUPPORTED=n
CONFIG_SOC_WDT_SUPPORTED=n
CONFIG_ESP_ROM_HAS_HAL_WDT=n
CONFIG_HAL_WDT_USE_ROM_IMPL=n

"/home/jim/src/arduino-esp32/tools/xtensa-esp-elf/bin/xtensa-esp32s3-elf-g++"  -MMD -c "@/home/jim/src/arduino-esp32/tools/esp32-arduino-libs/esp32s3/flags/cpp_flags" -w -Os -Werror=return-type -DF_CPU=240000000L -DARDUINO=10605 -DARDUINO_ESP32S3_DEV -DARDUINO_ARCH_ESP32 -DARDUINO_BOARD=\"ESP32S3_DEV\" -DARDUINO_VARIANT=\"esp32s3\" -DARDUINO_PARTITION_default -DARDUINO_HOST_OS=\"Linux\" -DARDUINO_FQBN=\"generic\" -DESP32=ESP32 -DCORE_DEBUG_LEVEL=0    -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MSC_ON_BOOT=0 -DARDUINO_USB_DFU_ON_BOOT=0  -DBOARD_HAS_PSRAM  -DGIT_VERSION=\""432271-dirty"\" -Ofast "@/home/jim/src/arduino-esp32/tools/esp32-arduino-libs/esp32s3/flags/defines" "-I/home/jim/src/espEmptyTemplate/" -iprefix "/home/jim/src/arduino-esp32/tools/esp32-arduino-libs/esp32s3/include/" "@/home/jim/src/arduino-esp32/tools/esp32-arduino-libs/esp32s3/flags/includes" "-I/home/jim/src/arduino-esp32/tools/esp32-arduino-libs/esp32s3/qio_opi/include"  -I/home/jim/src/arduino-esp32/cores/esp32 -I/home/jim/src/arduino-esp32/variants/esp32s3 -I/tmp/mkESP/espEmptyTemplate_esp32s3 -I/home/jim/src/espEmptyTemplate -I/home/jim/src/arduino-esp32/libraries/LittleFS/src -I/home/jim/src/arduino-esp32/libraries/ESPmDNS/src -I/home/jim/src/arduino-esp32/libraries/HTTPClient/src -I/home/jim/src/arduino-esp32/libraries/Wire/src -I/home/jim/src/arduino-esp32/libraries/NetworkClientSecure/src -I/home/jim/src/arduino-esp32/libraries/FS/src -I/home/jim/src/arduino-esp32/libraries/ArduinoOTA/src -I/home/jim/src/arduino-esp32/libraries/Update/src -I/home/jim/src/arduino-esp32/libraries/Network/src -I/home/jim/src/arduino-esp32/libraries/WiFi/src -I/home/jim/Arduino/libraries/DHT_sensor_library -I/home/jim/Arduino/libraries/PubSubClient/src -I/home/jim/Arduino/libraries/Adafruit_Unified_Sensor -I/home/jim/Arduino/libraries/Arduino_CRC32/src -I/home/jim/Arduino/libraries/OneWireNg/src -I/home/jim/Arduino/libraries/OneWireNg/src/platform -I/home/jim/Arduino/libraries/Adafruit_HX711 -I/home/jim/Arduino/libraries/ArduinoJson -I/home/jim/Arduino/libraries/ArduinoJson/src -I/home/jim/Arduino/libraries/ArduinoJson/extras/tests/Helpers -I/home/jim/Arduino/libraries/ArduinoJson/extras/tests/Helpers/api -I/home/jim/Arduino/libraries/ArduinoJson/extras/tests/Helpers/avr -I/home/jim/Arduino/libraries/esp32jimlib/src "@/tmp/mkESP/espEmptyTemplate_esp32s3/build_opt.h" "@/tmp/mkESP/espEmptyTemplate_esp32s3/file_opts"    /tmp/mkESP/espEmptyTemplate_esp32s3/espEmptyTemplate.ino.cpp -S /tmp/asm.S

#endif
