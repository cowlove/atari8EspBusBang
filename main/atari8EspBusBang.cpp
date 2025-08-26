#pragma GCC optimize("O1")
#ifdef ARDUINO
#include "Arduino.h"
#endif
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
#include <driver/dedic_gpio.h>
#include <xtensa_timer.h>
#include <xtensa_rtos.h>
#include "rom/ets_sys.h"
#include "soc/dport_access.h"
#include "soc/system_reg.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "spiffs.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include <deque>
#include <functional>
#include <algorithm>
#include <inttypes.h>

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
#ifndef ARDUINO
#include "arduinoLite.h"
#endif

void sendHttpRequest();
void connectWifi();
void connectToServer();
void start_webserver(void);

// boot SDX cartridge image - not working well enough to base stress tests on it 
#define BOOT_SDX

#define XE_BANK
#ifndef BOOT_SDX
//#define RAMBO_XL256
#endif

spiffs *spiffs_fs = NULL;

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

// TMP: investigae removing these, should be unneccessary due to linker script
#undef DRAM_ATTR
#define DRAM_ATTR
#undef IRAM_ATTR
#define IRAM_ATTR 


unsigned IRAM_ATTR my_nmi(unsigned x) { return 0; }

IRAM_ATTR inline void delayTicks(int ticks) { 
    uint32_t startTsc = XTHAL_GET_CCOUNT();
    while(XTHAL_GET_CCOUNT() - startTsc < ticks) {}
}

DRAM_ATTR uint32_t bmonArray[bmonArraySz] = {0};
DRAM_ATTR volatile unsigned int bmonHead = 0;
DRAM_ATTR volatile unsigned int bmonTail = 0;
DRAM_ATTR unsigned int bmonMax = 0;
DRAM_ATTR unsigned int mmuChangeBmonMaxEnd = 0;
DRAM_ATTR unsigned int mmuChangeBmonMaxStart = 0;

DRAM_ATTR RAM_VOLATILE uint8_t *banks[nrBanks * 4];
DRAM_ATTR uint32_t bankEnable[nrBanks * 4];
DRAM_ATTR RAM_VOLATILE uint8_t atariRam[baseRamSz] = {0x0};
DRAM_ATTR uint8_t *xeBankMem[16] = {0};
DRAM_ATTR RAM_VOLATILE uint8_t dummyRam[bankSize] = {0x0};
DRAM_ATTR RAM_VOLATILE uint8_t D000Write[0x600] = {0x0};
DRAM_ATTR RAM_VOLATILE uint8_t D000Read[0x600] = {0xff};
DRAM_ATTR RAM_VOLATILE uint8_t pbiROM[2 * 1024] = {
#include "pbirom.h"
};
#if 0 
DRAM_ATTR RAM_VOLATILE uint8_t page6Prog[] = {
#include "page6.h"
};
#endif
DRAM_ATTR uint8_t diskImg[] = {
//#include "disk.h"
};
#if 0
DRAM_ATTR uint8_t cartROM[] = {
    #include "joust.h"
};
#endif 
struct BmonTrigger { 
    uint32_t mask;
    uint32_t value;
    uint32_t mark; 
    int depth;
    int preroll;
    int count;
    int skip;
};

DRAM_ATTR struct {
    uint32_t mask;
    uint32_t value;
} bmonExcludes[] = { 
#if 1
    {
        .mask = (refreshMask) << bmonR0Shift,                            // ignore refresh bus traffic  
        .value = (0) << bmonR0Shift,
    },
    {
        .mask = (readWriteMask | (0xf400 << addrShift)) << bmonR0Shift,   // ignore reads from char map  
        .value = (readWriteMask | (0xe000 << addrShift)) << bmonR0Shift,
    },
    {
        .mask = (readWriteMask | (0xffff << addrShift)) << bmonR0Shift,   // ignore reads from 0x00ff
        .value = (readWriteMask | (0x00ff << addrShift)) << bmonR0Shift,
    },
    {
        .mask = (readWriteMask | (0xf800 << addrShift)) << bmonR0Shift,   // ignore reads from screen mem
        .value = (readWriteMask | (0xb800 << addrShift)) << bmonR0Shift,
    },
#endif
};

//DRAM_ATTR volatile vector<BmonTrigger> bmonTriggers = {
DRAM_ATTR BmonTrigger bmonTriggers[] = {/// XXTRIG 
#if 0 //TODO why does this trash the bmon timings?
    { 
        .mask =  (((0 ? readWriteMask : 0) | (0xff00 << addrShift)) << bmonR0Shift) | (0x00), 
        .value = (((0 ? readWriteMask : 0) | (0xd500 << addrShift)) << bmonR0Shift) | (0x00),
        .mark = 0,
        .depth = 10,
        .preroll = 5,
        .count = INT_MAX,
        .skip = 0 // TODO - doesn't work? 
    },
#endif
#if 0 
    { 
        .mask =  (readWriteMask | (0xffff << addrShift)) << bmonR0Shift, 
        .value = (0             | (0xd301 << addrShift)) << bmonR0Shift,
        .mark = 0,
        .depth = 2,
        .preroll = 0,
        .count = INT_MAX,
        .skip = 0 // TODO - doesn't work? 
    },
#endif
#if 0 // TODO: too many bmonTriggers slows down IO and hangs the system
    { /// XXTRIG
        .mask = (readWriteMask | (0xffff << addrShift)) << bmonR0Shift, 
        .value = (readWriteMask | (0xfffa << addrShift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (readWriteMask | (0xffff << addrShift)) << bmonR0Shift, 
        .value = (readWriteMask | (0xfffe << addrShift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (readWriteMask | (0xffff << addrShift)) << bmonR0Shift, 
        .value = (readWriteMask | (39968 << addrShift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (readWriteMask | (0xffff << addrShift)) << bmonR0Shift, 
        .value = (readWriteMask | (0xfffa << addrShift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (readWriteMask | (0xffff << addrShift)) << bmonR0Shift, 
        .value = (readWriteMask | (0xfffe << addrShift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (readWriteMask | (0xffff << addrShift)) << bmonR0Shift, 
        .value = (readWriteMask | (39968 << addrShift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
#endif
};
BUSCTL_VOLATILE DRAM_ATTR uint32_t busMask = extSel_Mask;
DRAM_ATTR BUSCTL_VOLATILE uint32_t pinDisableMask = interruptMask | dataMask | extSel_Mask | mpdMask;
DRAM_ATTR BUSCTL_VOLATILE uint32_t pinInhibitMask = ~0;

DRAM_ATTR uint32_t busEnabledMark;
DRAM_ATTR BUSCTL_VOLATILE uint32_t pinEnableMask = 0;
DRAM_ATTR int busWriteDisable = 0;

DRAM_ATTR int ioCount = 0, pbiInterruptCount = 0, memWriteErrors = 0, unmapCount = 0, 
    watchDogCount = 0, spuriousHaltCount = 0, haltCount = 0;
DRAM_ATTR string exitReason = "";
DRAM_ATTR int elapsedSec = 0;
DRAM_ATTR int exitFlag = 0;
DRAM_ATTR uint32_t lastVblankTsc = 0;

#define CAR_FILE_MAGIC  ((int)'C' + ((int)'A' << 8) + ((int)'R' << 16) + ((int)'T' << 24))
struct __attribute__((packed)) CARFileHeader {
    uint32_t magic = 0;
    uint8_t unused[3];
    uint8_t type = 0; 
    uint32_t cksum;
    uint32_t unused2;
};

struct DRAM_ATTR AtariCart {
    enum CarType { 
        None = 0,
        AtMax128 = 41,
        Std8K = 1,
        Std16K = 2,
    };
    string filename;
    CARFileHeader header;
    uint8_t **image = NULL;
    size_t size = 0;
    int bankCount = 0;
    int type = -1;
    int bank80 = -1, bankA0 = -1;
    void IFLASH_ATTR open(const char *f);
    bool IRAM_ATTR inline accessD500(uint16_t addr) {
        int b = (addr & 0xff); 
        if (image != NULL && b != bankA0 && (b & 0xe0) == 0) { 
            bankA0 = b < bankCount ? b : -1;
            return true;
        }
        return false;
    }
} atariCart;

void IFLASH_ATTR AtariCart::open(const char *f) {
    spiffs_file fd;
    bank80 = bankA0 = -1;
    bankCount = 0;

    spiffs_stat stat;
    if (SPIFFS_stat(spiffs_fs, f, &stat) < 0 ||
        (fd = SPIFFS_open(spiffs_fs, f, SPIFFS_O_RDONLY, 0)) < 0) { 
        printf("AtariCart::open('%s'): file open failed\n", f);
        return;
    }
    size_t fsize = stat.size;
    if ((fsize & 0x1fff) == sizeof(header)) {
        int r = SPIFFS_read(spiffs_fs, fd, &header, sizeof(header));
        if (r != sizeof(header) || 
            (header.type != AtMax128 
                && header.type != Std8K
                && header.type != Std16K) 
            /*|| header.magic != CAR_FILE_MAGIC */) { 
            SPIFFS_close(spiffs_fs, fd);
            printf("AtariCart::open('%s'): bad file, header, or type\n", f);
            return;
        }
        size = fsize - sizeof(header);
    } else { 
        size = fsize;
        if (size == 0x2000) header.type = Std8K;
        else if (size == 0x4000) header.type = Std16K;
        else {
            SPIFFS_close(spiffs_fs, fd);
            printf("AtariCart::open('%s'): raw ROM file isn't 8K or 16K in size\n", f);
            return;
        }
    }
    printf("AtariCart::open('%s'): ROM size %d\n", f, size); 

    // TODO: malloc 8k banks instead of one large chunk
    bankCount = size >> 13;
    image = (uint8_t **)heap_caps_malloc(bankCount * sizeof(uint8_t *), MALLOC_CAP_INTERNAL);
    if (image == NULL) {
        printf("AtariCart::open('%s'): dram heap_caps_malloc() failed!\n", f);
        return;
    }            
    for (int i = 0; i < bankCount; i++) {
        image[i] = (uint8_t *)heap_caps_malloc(0x2000, MALLOC_CAP_INTERNAL);
        if (image[i] == NULL) {
            printf("AtariCart::open('%s'): dram heap_caps_malloc() failed bank %d!\n", f, i);
            heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
            while(--i > 0)
                heap_caps_free(image[i]);
            heap_caps_free(image);
            return;
        }
        int r = SPIFFS_read(spiffs_fs, fd, image[i], 0x2000);
    }
    SPIFFS_close(spiffs_fs, fd);
    if (header.type == Std16K) {
        bank80 = 0;
        bankA0 = 1;
    } else { 
        bankA0 = 0;
        bank80 = -1;
    }
    bankCount = size >> 13;
}   

static const DRAM_ATTR struct {
    uint8_t osEn = 0x1;
    uint8_t basicEn = 0x2;
    uint8_t selfTestEn = 0x80;    
    uint8_t xeBankEn = 0x10;
} portbMask;

inline IRAM_ATTR void mmuUnmapRange(uint16_t start, uint16_t end) { 
    for(int b = bankNr(start); b <= bankNr(end); b++) { 
        banks[b + BANKSEL_WR + BANKSEL_CPU] = &dummyRam[0];
        bankEnable[b + BANKSEL_CPU + BANKSEL_RD] = 0;
    }
}

inline IRAM_ATTR void mmuMapRange(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int b = bankNr(start); b <= bankNr(end); b++) { 
        banks[b + BANKSEL_WR + BANKSEL_CPU] = mem + (b - bankNr(start)) * bankSize;
        bankEnable[b + BANKSEL_CPU + BANKSEL_RD] = dataMask | extSel_Mask;
    }
}

inline IRAM_ATTR void mmuMapRangeRW(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int b = bankNr(start); b <= bankNr(end); b++) { 
        banks[b + BANKSEL_WR + BANKSEL_CPU] = mem + (b - bankNr(start)) * bankSize;
        banks[b + BANKSEL_RD + BANKSEL_CPU] = mem + (b - bankNr(start)) * bankSize;
        bankEnable[b + BANKSEL_CPU + BANKSEL_RD] = dataMask | extSel_Mask;
    }
}

inline IRAM_ATTR void mmuMapRangeRO(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int b = bankNr(start); b <= bankNr(end); b++) { 
        banks[b + BANKSEL_WR + BANKSEL_CPU] = &dummyRam[0];
        banks[b + BANKSEL_RD + BANKSEL_CPU] = mem + (b - bankNr(start)) * bankSize;
        bankEnable[b + BANKSEL_CPU + BANKSEL_RD] = dataMask | extSel_Mask;
    }
}

inline IRAM_ATTR void mmuUnmapRangeRW(uint16_t start, uint16_t end) { 
    for(int b = bankNr(start); b <= bankNr(end); b++) { 
        banks[b + BANKSEL_WR + BANKSEL_CPU] = &dummyRam[0];
        banks[b + BANKSEL_RD + BANKSEL_CPU] = &dummyRam[0];
        bankEnable[b + BANKSEL_CPU + BANKSEL_RD] = 0;
    }
}

inline IRAM_ATTR void mmuMapPbiRom(bool pbiEn, bool osEn) {
    if (pbiEn) {
        mmuMapRangeRW(0xd800, 0xdfff, &pbiROM[0]);
    } else if(osEn || baseRamSz < 64 * 1024) {
        mmuUnmapRangeRW(0xd800, 0xdfff);
    } else {
        mmuMapRangeRW(0xd800, 0xdfff, &atariRam[0xd800]);
    }
    if (pbiEn) { 
        pinDisableMask &= (~mpdMask);
        pinEnableMask |= mpdMask;
    } else { 
        pinDisableMask |= mpdMask;
        pinEnableMask &= (~mpdMask);
    }
}

// Called any time values in portb(0xd301) or newport(0xd1ff) change
IRAM_ATTR void onMmuChange(bool force = false) {
    uint32_t stsc = XTHAL_GET_CCOUNT();
    mmuChangeBmonMaxStart = max((bmonHead - bmonTail) & (bmonArraySz - 1), mmuChangeBmonMaxStart); 
    uint8_t newport = D000Write[0x1ff];
    uint8_t portb = D000Write[0x301]; 

    static bool lastBasicEn = true;
    static bool lastPbiEn = false;
    static bool lastPostEn = false;
    static bool lastOsEn = true;
    static bool lastXeBankEn = false;
    static int lastXeBankNr = 0;
    static int lastBankA0 = -1, lastBank80 = -1;

#ifdef XE_BANK
    bool xeBankEn = (portb & portbMask.xeBankEn) == 0;
    int xeBankNr = ((portb & 0x60) >> 3) | ((portb & 0x0c) >> 2); 
    if (lastXeBankEn != xeBankEn ||  lastXeBankNr != xeBankNr || force) { 
        if (xeBankEn) { 
            mmuMapRangeRW(0x4000, 0x7fff, xeBankMem[xeBankNr]);
        } else { 
            mmuMapRangeRW(0x4000, 0x7fff, &atariRam[0x4000]);
        }
        lastXeBankEn = xeBankEn;
        lastXeBankNr = xeBankNr;
    }
#endif

    bool osEn = (portb & portbMask.osEn) != 0;
    bool pbiEn = (newport & pbiDeviceNumMask) != 0;
    if (baseRamSz == 64 * 1024 && (lastOsEn != osEn || force)) { 
        if (osEn) {
            mmuUnmapRange(0xe000, 0xffff);
#if bankSz <= 0x200
            //mmuUnmapRange(0xd600, 0xd7ff);
#endif
            mmuUnmapRange(0xc000, 0xcfff);
        } else { 
            mmuMapRange(0xe000, 0xffff, &atariRam[0xe000]);
#if bankSz <= 0x200
            //mmuMapRange(0xd600, 0xd7ff, &atariRam[0xd600]);
#endif
            mmuMapRange(0xc000, 0xcfff, &atariRam[0xc000]);
        }
        //mmuMapPbiRom(pbiEn, osEn);
        //lastPbiEn = pbiEn;
        lastOsEn = osEn;
    }

    if (pbiEn != lastPbiEn || force) {
        mmuMapPbiRom(pbiEn, osEn);
        lastPbiEn = pbiEn;
    }

    bool postEn = (portb & portbMask.selfTestEn) == 0;
    if (lastPostEn != postEn || force) { 
        if (postEn) {
            mmuUnmapRange(0x5000, 0x57ff);
        } else {
            mmuMapRange(0x5000, 0x57ff, &atariRam[0x5000]);
        }
        lastPostEn = postEn;
    }

    bool basicEn = (portb & portbMask.basicEn) == 0;
    if (lastBasicEn != basicEn || lastBankA0 != atariCart.bankA0 || force) { 
        if (basicEn) { 
            mmuUnmapRange(0xa000, 0xbfff);
        } else if (atariCart.bankA0 >= 0) {
            mmuMapRangeRO(0xa000, 0xbfff, atariCart.image[atariCart.bankA0]);
        } else { 
            mmuMapRange(0xa000, 0xbfff, &atariRam[0xa000]);
        }
        lastBasicEn = basicEn;
        lastBankA0 = atariCart.bankA0;
    }
    if (lastBank80 != atariCart.bank80 || force) { 
        if (atariCart.bank80 >= 0) {
            mmuMapRangeRO(0x8000, 0x9fff, atariCart.image[atariCart.bank80]);
        } else { 
            mmuMapRange(0x8000, 0x9fff, &atariRam[0x8000]);
        }
        lastBank80 = atariCart.bank80;
    }
}

IFLASH_ATTR void memoryMapInit() { 
    bzero(bankEnable, sizeof(bankEnable));
    mmuUnmapRangeRW(0x0000, 0xffff);

    // map all banks to atariRam array 
    mmuMapRangeRW(0x0000, baseRamSz - 1, &atariRam[0x0000]);
    mmuUnmapRangeRW(0xd000, 0xd5ff);
    mmuUnmapRangeRW(0xd600, 0xd7ff);
    //mmuUnmapRangeRW(0xd800, 0xdfff);

    // map register writes for banks d000-d7ff to shadow write banks
    for(int b = bankNr(0xd000); b <= bankNr(0xd7ff); b++) { 
        banks[b | BANKSEL_CPU | BANKSEL_WR ] = &D000Write[0] + (b - bankNr(0xd000)) * bankSize; 
    }

    // TODO: investigate setting data pin output to open-drain, allowing parts of a page to effectively be 
    // "unmapped" by setting the read data to 0xff.  This would allow only the 0xd1ff byte of the register
    // read page to be "mapped", allowing page sizes of up to 2k while still supporting interrupts

#if bankSize <= 0x100    
    // Map register reads for the bank containing 0xd1ff so we can handle reads to newport/0xd1ff for implementing
    // PBI interrupt scheme 
//    banks[bankNr(0xd1ff) | BANKSEL_CPU | BANKSEL_RD ] = &D000Read[(bankNr(0xd1ff) - bankNr(0xd000)) * bankSize]; 
//    bankEnable[bankNr(0xd1ff) | BANKSEL_CPU | BANKSEL_RD] = dataMask | extSel_Mask;
//    bankEnable[bankNr(0xd500) | BANKSEL_CPU | BANKSEL_RD ] |= haltMask;
#endif

    // enable the halt(ready) line in response to writes to 0xd301 or 0xd500
    bankEnable[bankNr(0xd300) | BANKSEL_CPU | BANKSEL_WR ] |= haltMask;
    bankEnable[bankNr(0xd500) | BANKSEL_CPU | BANKSEL_WR ] |= haltMask;

    // TODO: investigate cartridge mapping registers  

    // Intialize register shadow write memory to the default hardware reset values
    D000Write[0x301] = 0xff;
    D000Write[0x1ff] = 0x00;

    onMmuChange(/*force =*/true);
}

DRAM_ATTR int deferredInterrupt = 0, interruptRequested = 0, sysMonitorRequested = 0;
IRAM_ATTR void busyWaitTicks(uint32_t ticks) { 
    uint32_t tsc = XTHAL_GET_CCOUNT();
    while(XTHAL_GET_CCOUNT() - tsc < ticks) {};
}
IRAM_ATTR void busyWait6502Ticks(uint32_t ticks) { 
    uint32_t tsc = XTHAL_GET_CCOUNT();
    static const DRAM_ATTR int ticksPer6502Tick = 132;
    while(XTHAL_GET_CCOUNT() - tsc < ticks * ticksPer6502Tick) {};
}
IRAM_ATTR void busywait(float sec) {
    uint32_t tsc = XTHAL_GET_CCOUNT();
    static const DRAM_ATTR int cpuFreq = 240 * 1000000;
    while(XTHAL_GET_CCOUNT() - tsc < sec * cpuFreq) {};
}

IRAM_ATTR void raiseInterrupt() {
    if ((D000Write[0x1ff] & pbiDeviceNumMask) != pbiDeviceNumMask
        && (D000Write[0x301] & 0x1) != 0
    ) {
        deferredInterrupt = 0;  
        D000Read[0x1ff] = pbiDeviceNumMask;
        atariRam[PDIMSK] |= pbiDeviceNumMask;
        pinDisableMask &= (~interruptMask);
        pinEnableMask |= interruptMask;
        interruptRequested = 1;
    } else { 
        deferredInterrupt = 1;
    }
}

IRAM_ATTR void clearInterrupt() { 
    pinEnableMask &= (~interruptMask);
    pinDisableMask |= interruptMask;
    interruptRequested = 0;
    busyWait6502Ticks(10);
    D000Read[0x1ff] = 0x0;
    atariRam[PDIMSK] &= (~pbiDeviceNumMask);
}

IRAM_ATTR void enableBus() {
    busWriteDisable = 0;
    pinInhibitMask = ~0; 
}

IRAM_ATTR void disableBus() { 
    busWriteDisable = 1;
    pinInhibitMask = haltMask;
}

IRAM_ATTR std::string vsfmt(const char *format, va_list args);
IRAM_ATTR std::string sfmt(const char *format, ...);
class LineBuffer {
public:
        char line[128];
        int len = 0;
        int IRAM_ATTR add(char c, std::function<void(const char *)> f = NULL);
        void IRAM_ATTR add(const char *b, int n, std::function<void(const char *)> f);
        void IRAM_ATTR add(const uint8_t *b, int n, std::function<void(const char *)> f);
};

DRAM_ATTR static const int psram_sz =  128 * 1024;
DRAM_ATTR uint32_t *psram;
DRAM_ATTR uint32_t *psram_end;

DRAM_ATTR static const int testFreq = 1.78 * 1000000;//1000000;
DRAM_ATTR static const int lateThresholdTicks = 180 * 2 * 1000000 / testFreq;
static const DRAM_ATTR uint32_t halfCycleTicks = 240 * 1000000 / testFreq / 2;
DRAM_ATTR int wdTimeout = 0, ioTimeout = 40;
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

struct __attribute__((packed)) AtariIOCB { 
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

const DRAM_ATTR struct AtariDefStruct {
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
#ifdef BOOT_SDX
        "10 PRINT \"HELLO FROM BASIC\" \233"
        "20 PRINT \"HELLO 2\" \233"
        "30 CLOSE #4:OPEN #4,4,0,\"J1:DUMPSCREEN\":CLOSE #4\233"
        "40 DOS\233 "
        "RUN \233"
#else
        "1 DIM D$(255) \233"
        "10 REM A=USR(1546, 1) \233"
        "15 OPEN #1,4,0,\"J2:\" \233"
        "20 GET #1,A  \233"
        "38 CLOSE #1  \233"
        //"40 GOTO 10 \233"
        "41 OPEN #1,8,0,\"J\" \233"
        "42 PUT #1,A + 1 \233"
        "43 CLOSE #1 \233"
        "50 PRINT \" -> \"; \233"
        "52 PRINT COUNT; \233"
        "53 COUNT = COUNT + 1 \233"
        "60 OPEN #1,8,0,\"D1:DAT\":FOR I=0 TO 20:XIO 11,#1,8,0,D$:NEXT I:CLOSE #1 \233"
        //"61 TRAP 61: CLOSE #1: OPEN #1,4,0,\"D1:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        "61 CLOSE #1: OPEN #1,4,0,\"D1:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        //"63 OPEN #1,4,0,\"D2:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        "70 TRAP 80:XIO 80,#1,0,0,\"D1:X.CMD\" \233"
        "80 GOTO 10 \233"
        "RUN\233"
#endif
        ;

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
} simulatedKeyInput;

DRAM_ATTR static int lastScreenShot = 0;
DRAM_ATTR int secondsWithoutWD = 0, lastIoSec = 0;

void IFLASH_ATTR dumpScreenToSerial(char tag);

// CORE0 loop options 
struct AtariIO {
    uint8_t buf[2048];
    int ptr = 0;
    int len = 0;
    AtariIO() { 
        strcpy((char *)buf, defaultProgram); 
        len = strlen((char *)buf);
    }
    inline IRAM_ATTR void open(const char *fn) { 
        ptr = 0; 
        watchDogCount++;
        if (strcmp(fn, DRAM_STR("J1:DUMPSCREEN")) == 0) { 
            dumpScreenToSerial('S');
            lastScreenShot = elapsedSec;
        }
    }
    inline IRAM_ATTR void close() {}
    inline IRAM_ATTR int get() { 
        if (ptr >= len) return -1;
        return buf[ptr++];
    }
    inline IRAM_ATTR int put(uint8_t c) { 
        return 1;
    }
};
DRAM_ATTR AtariIO fakeFile; 

struct __attribute__((packed)) AtariDCB { 
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

struct __attribute__((packed)) PbiIocb {
    uint8_t req;
    uint8_t cmd;
    uint8_t a;
    uint8_t x;

    uint8_t y;
    uint8_t carry;
    uint8_t result;
    uint8_t psp;

    uint8_t nmien;
    uint8_t rtclok1;
    uint8_t rtclok2;
    uint8_t rtclok3;

    uint8_t kbcode;
    uint8_t sdmctl;
    uint8_t stackprog;
    uint8_t consol;
};

#define STRUCT_LOG
#ifdef STRUCT_LOG 
template<class T> 
struct StructLog { 
    int maxSize;
    uint32_t lastTsc;
    StructLog(int maxS = 32) : maxSize(maxS) {}
    std::deque<std::pair<uint32_t,T>> log;
    inline void /*IRAM_ATTR*/ add(const T &t) {
        uint32_t tsc = XTHAL_GET_CCOUNT(); 
        log.push_back(std::pair<uint32_t,T>(tsc - lastTsc, t));
        lastTsc = tsc;
        if (log.size() > maxSize) log.pop_front();
    }
    static inline /*IRAM_ATTR*/ void printEntry(const T&);
    inline void /*IRAM_ATTR*/ print() { 
        for(auto a : log) {
            printf(DRAM_STR("%-10" PRIu32 ": "), a.first);
            printEntry(a.second);
        } 
    }
};
template <class T> inline /*IRAM_ATTR*/ void StructLog<T>::printEntry(const T &a) {
    for(int i = 0; i < sizeof(a); i++) printf(DRAM_STR("%02x "), ((uint8_t *)&a)[i]);
    printf(DRAM_STR("\n"));
}
template <> inline void StructLog<string>::printEntry(const string &a) { 
    printf(DRAM_STR("%s\n"), a.c_str()); 
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
    StructLog<AtariDCB> dcb = StructLog<AtariDCB>(200); 
    StructLog<AtariIOCB> iocb; 
    StructLog<PbiIocb> pbi = StructLog<PbiIocb>(50);
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
struct __attribute__((packed)) AtrImageHeader {
    uint16_t magic; // 0x0296;
    uint16_t pars;  // disk image size divided by 0x10
    uint16_t sectorSize; // usually 0x80 or 0x100
    uint8_t parsHigh; // high byte of larger wPars size (added in rev3.00)
    uint32_t crc;       
    uint32_t unused;
    uint8_t flags;
};

struct DiskImage {
    string filename;
    AtrImageHeader header;
    uint8_t *image;
    spiffs_file fd;
    void open(const char *f, bool cacheInPsram) {
        image = NULL;
        spiffs_stat stat;
        if (SPIFFS_stat(spiffs_fs, f, &stat) < 0 ||
            (fd = SPIFFS_open(spiffs_fs, f, SPIFFS_O_RDWR, 0)) < 0) { 
            printf("AtariDisk::open('%s'): file open failed\n", f);
            return;
        }
        size_t fsize = stat.size;
        int r = SPIFFS_read(spiffs_fs, fd, &header, sizeof(header));
        if (r != sizeof(header) || header.magic != 0x0296) { 
            printf("DiskImage::open('%s'): bad file or bad atr header\n", f);
            SPIFFS_close(spiffs_fs, fd);
            return;
        }
        printf("DiskImage::open('%s'): sector size %d, header size %d\n", 
            f, header.sectorSize, sizeof(header));
        if (cacheInPsram) { 
            size_t dataSize = (header.pars + header.parsHigh * 0x10000) * 0x10;
            image = (uint8_t *)heap_caps_malloc(dataSize, MALLOC_CAP_SPIRAM);
            if (image == NULL) {
                printf("DiskImage::open('%s'): psram heap_caps_malloc(%d) failed!\n", f, dataSize);
                heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
                SPIFFS_close(spiffs_fs, fd);
                return;
            }
            printf("Opened '%s' file size %zu bytes, reading data: ", f, fsize);
            SPIFFS_lseek(spiffs_fs, fd, sizeof(header), SPIFFS_SEEK_SET);
            int r = SPIFFS_read(spiffs_fs, fd, image, dataSize);
            SPIFFS_close(spiffs_fs, fd); 
            if (r != dataSize) { 
                printf("wrong size, discarding\n");
                heap_caps_free(image);
                image = NULL;
                return;
            }
            printf("OK\n");
        } 
        filename = f;
    }
    bool IRAM_ATTR valid() { return header.magic == 0x0296; }
    void IRAM_ATTR close() {
        if (image != NULL) {
            heap_caps_free(image);
            image = NULL;
        } else { 
            SPIFFS_close(spiffs_fs, fd); 
        }
    }
    size_t IRAM_ATTR read(uint8_t *buf, size_t offset, size_t len) {
        if(image != NULL) {
            for(int n = 0; n < len; n++) 
                buf[n] = image[offset + n];
            return len;
        } else if(filename.length() != 0) {
            SPIFFS_lseek(spiffs_fs, fd, offset + sizeof(header), SPIFFS_SEEK_SET);
            return SPIFFS_read(spiffs_fs, fd, buf, len);                                    
        }
        return 0;
    }
    size_t IRAM_ATTR write(uint8_t *buf, size_t offset, size_t len) { 
        if(image != NULL) {
            for(int n = 0; n < len; n++) image[offset + n] = buf[n];
            return len;
        } else if(filename.length() != 0) {
            //printf("write %d at %d:\n", len, offset); 
            SPIFFS_lseek(spiffs_fs, fd, offset + sizeof(header), SPIFFS_SEEK_SET);
            size_t r = SPIFFS_write(spiffs_fs, fd, buf, len);  
            //SPIFFS_flush(spiffs_fs, fd);
            return r;
        }
        return 0;
    }
};

DRAM_ATTR DiskImage atariDisks[8];

struct ScopedInterruptEnable { 
    IRAM_ATTR ScopedInterruptEnable() { 
        unmapCount++;
        disableBus();
        busyWait6502Ticks(2);
        enableCore0WDT();
        portENABLE_INTERRUPTS();
        yield();
    }
    IRAM_ATTR ~ScopedInterruptEnable() {
        yield();
        portDISABLE_INTERRUPTS();
        disableCore0WDT();
        busyWait6502Ticks(20); // wait for core1 to stabilize again 
        //bmonTail = bmonHead;
        enableBus();

    }
};

// Macro gibberish to define EVERYN_TICKS(n) { /*code executed once every n ticks */ }
#define TOKEN_PASTE(A, B) A##B
#define CONCAT_HELPER(A, B) TOKEN_PASTE(A,B)
#define UNIQUE_LOCAL(A) CONCAT_HELPER(A, __LINE__)
#define EVERYN_TICKS(ticks) \
    static DRAM_ATTR uint32_t UNIQUE_LOCAL(lastTsc) = XTHAL_GET_CCOUNT(); \
    static const DRAM_ATTR uint32_t UNIQUE_LOCAL(interval) = (ticks); \
    const uint32_t UNIQUE_LOCAL(tsc) = XTHAL_GET_CCOUNT(); \
    bool UNIQUE_LOCAL(doLoop) = false; \
    if(UNIQUE_LOCAL(tsc) - UNIQUE_LOCAL(lastTsc) > \
        UNIQUE_LOCAL(interval)) {\
        UNIQUE_LOCAL(lastTsc) = UNIQUE_LOCAL(tsc); \
        UNIQUE_LOCAL(doLoop) = true; \
    } \
    if (UNIQUE_LOCAL(doLoop))


bool IRAM_ATTR needSafeWait(PbiIocb *pbiRequest) {
    if (pbiRequest->req != 2) {
        pbiRequest->result = 2;
        return true;
    } 
    return false;
}
//#define SCOPED_INTERRUPT_ENABLE(pbiReq) if (needSafeWait(pbiReq)) return; ScopedInterruptEnable intEn;  
#define SCOPED_INTERRUPT_ENABLE(pbiReq) ScopedInterruptEnable intEn;  

struct SysMonitorMenuItem {
    string text;
    std::function<void(bool)> onSelect;
};

class SysMonitorMenu {
public:
    vector<SysMonitorMenuItem> options;
    int selected = 0;
    SysMonitorMenu(const vector<SysMonitorMenuItem> &v) : options(v) {}
};

struct Debounce { 
    int last = 0;
    int stableTime = 0;
    int lastStable = 0;
    int debounceDelay;
    Debounce(int d) : debounceDelay(d) {}
    inline void IRAM_ATTR reset(int val) { lastStable = val; }
    inline bool IRAM_ATTR debounce(int val, int elapsed = 1) { 
        if (val == last) {
            stableTime += elapsed;
        } else {
            last = val; 
            stableTime = 0;
        }
        if (stableTime >= debounceDelay && val != lastStable) {
            lastStable = val;
            return true;
        }
        return false;
    }
};

class SysMonitor {
    SysMonitorMenu menu = SysMonitorMenu({
        {"OPTION 1", [](bool) {}}, 
        {"SECOND OPTION", [](bool) {}}, 
        {"LAST", [](bool){}},
    });
    float activeTimeout = 0;
    bool exitRequested = false;
    uint8_t screenMem[24 * 40];
    void IRAM_ATTR  saveScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        for(int i = 0; i < sizeof(screenMem); i++) { 
            screenMem[i] = atariRam[savmsc + i];
        }
    }
    Debounce consoleDebounce = Debounce(240 * 1000 * 30);
    Debounce keyboardDebounce = Debounce(240 * 1000 * 30);

    void IRAM_ATTR  clearScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        for(int i = 0; i < sizeof(screenMem); i++) { 
            atariRam[savmsc + i] = 0;
         }
    }
    void IRAM_ATTR  drawScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        //atariRam[savmsc]++;
        //clearScreen();
        writeAt(-1, 2, DRAM_STR(" SYSTEM MONITOR "), true);
        writeAt(-1, 4, DRAM_STR("Everything will be fine!"), false);
        writeAt(-1, 5, sfmt(DRAM_STR("Timeout: %.0f"), activeTimeout), false);
        writeAt(-1, 7, sfmt(DRAM_STR("KBCODE = %02x CONSOL = %02x"), (int)pbiRequest->kbcode, (int)pbiRequest->consol), false);
        for(int i = 0; i < menu.options.size(); i++) {
            const int xpos = 5, ypos = 9; 
            const string cursor = DRAM_STR("-> ");
            writeAt(xpos, ypos + i, menu.selected == i ? cursor : DRAM_STR("   "), false);
            writeAt(xpos + cursor.length(), ypos + i, menu.options[i].text, menu.selected == i);
        }
        atariRam[712] = 255;
        atariRam[710] = 0;
    }
    void IRAM_ATTR  restoreScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        for(int i = 0; i < sizeof(screenMem); i++) { 
            atariRam[savmsc + i] = screenMem[i];
        }
        atariRam[712] = 0;
        atariRam[710] = 148;
    }
    void IRAM_ATTR  writeAt(int x, int y, const string &s, bool inv) { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        if (x < 0) x = 20 - s.length() / 2;
        for(int i = 0; i < s.length(); i++) { 
            uint8_t c = s[i];
            if (c < 32) c += 64;
            else if (c < 96) c-= 32;
            atariRam[savmsc + y * 40 + x + i] = c + (inv ? 128 : 0);            
        }
    }
    void IRAM_ATTR  onConsoleKey(uint8_t key) {
        if (key != 7) activeTimeout = 60;
        if (key == 6) menu.selected = min(menu.selected + 1, (int)menu.options.size() - 1);
        if (key == 3) menu.selected = max(menu.selected - 1, 0);
        if (key == 5) {};
        if (key == 0) exitRequested = true;
        if (key == 7 && exitRequested) activeTimeout = 0;
        //drawScreen();
    }
    public:
    PbiIocb *pbiRequest;
    uint32_t lastTsc;
    void IRAM_ATTR pbi(PbiIocb *p) {
        pbiRequest = p;
        uint32_t tsc = XTHAL_GET_CCOUNT(); 
        if (activeTimeout <= 0) { // first reactivation, reinitialize 
            lastTsc = tsc;
            activeTimeout = 1.0;
            if (pbiRequest->consol == 0 || pbiRequest->kbcode == 0xe5)
                activeTimeout = 5.0;
            exitRequested = false;
            menu.selected = 0;
            keyboardDebounce.reset(pbiRequest->kbcode);
            consoleDebounce.reset(pbiRequest->consol);
            saveScreen();
            clearScreen();
            //drawScreen();
        }
        uint32_t elapsedTicks = tsc - lastTsc;
        lastTsc = tsc; 
        if (activeTimeout > 0) {
            activeTimeout -= elapsedTicks / 240000000.0;
            if (consoleDebounce.debounce(pbiRequest->consol, elapsedTicks)) { 
                onConsoleKey(pbiRequest->consol);
            }
            if (keyboardDebounce.debounce(pbiRequest->kbcode, elapsedTicks)) { 
                //drawScreen();
            }
            drawScreen();
            pbiRequest->result |= 0x80;
        }
        if (activeTimeout <= 0) {
            pbiRequest->result &= (~0x80);
            restoreScreen();  
            activeTimeout = 0;
        }
    }
} DRAM_ATTR sysMonitor;

void IRAM_ATTR halt6502() { 
    pinEnableMask |= haltMask;
    busyWait6502Ticks(10);
    pinEnableMask &= (~haltMask);
}

void IRAM_ATTR resume6502() {
    haltCount++; 
    uint32_t stsc = XTHAL_GET_CCOUNT();
    pinDisableMask |= haltMask;
    int bHead = bmonHead;
    while(
        XTHAL_GET_CCOUNT() - stsc < bmonTimeout && 
        bmonHead == bHead) {
    }
    bHead = bmonHead;
    while(
        XTHAL_GET_CCOUNT() - stsc < bmonTimeout && 
        bmonHead == bHead) {
    }
    pinDisableMask &= (~haltMask);
}
void IFLASH_ATTR dumpScreenToSerial(char tag) {
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    printf(DRAM_STR("SCREEN%c 00 memory at SAVMSC(%04x):\n"), tag, savmsc);
    printf(DRAM_STR("SCREEN%c 01 +----------------------------------------+\n"), tag);
    for(int row = 0; row < 24; row++) { 
        printf(DRAM_STR("SCREEN%c %02d |"), tag, row + 2);
        for(int col = 0; col < 40; col++) { 
            uint16_t addr = savmsc + row * 40 + col;
            uint8_t c = atariRam[addr];
            bool inv = false;
            if (c & 0x80) {
                printf(DRAM_STR("\033[7m"));
                c -= 0x80;
                inv = true;
            };
            if (c < 64) c += 32;
            else if (c < 96) c -= 64;
            printf(DRAM_STR("%c"), c);
            if (inv) printf(DRAM_STR("\033[0m"));
        }
        printf(DRAM_STR("|\n"));
    }
    printf(DRAM_STR("SCREEN%c 27 +----------------------------------------+\n"), tag);
}

void IFLASH_ATTR handleSerial() {
    uint8_t c;
    while(usb_serial_jtag_read_bytes((void *)&c, 1, 0) > 0) { 
        static DRAM_ATTR LineBuffer lb;
        lb.add(c, [](const char *line) {
            char x;
            if (sscanf(line, DRAM_STR("key %c"), &x) == 1) {
                simulatedKeyInput.putKey(x);
            } else if (sscanf(line, DRAM_STR("exit %c"), &x) == 1) {
                exitFlag = x;
            } else if (sscanf(line, DRAM_STR("screen %c"), &x) == 1) {
                dumpScreenToSerial(x);
            }
        });
    }
}

void IRAM_ATTR handlePbiRequest2(PbiIocb *pbiRequest) {     
    // TMP: put the shortest, quickest interrupt service possible
    // here 
    structLogs.pbi.add(*pbiRequest);
    
    AtariIOCB *iocb = (AtariIOCB *)&atariRam[AtariDef.IOCB0 + pbiRequest->x]; // todo validate x bounds
    //pbiRequest->y = 1; // assume success
    //pbiRequest->carry = 0; // assume fail 
    if (pbiRequest->cmd == 1) { // open
        SCOPED_INTERRUPT_ENABLE(pbiRequest);
        pbiRequest->y = 1; // assume success
        pbiRequest->carry = 0; // assume fail 
        uint16_t addr = ((uint16_t )atariMem.ziocb->ICBAH) << 8 | atariMem.ziocb->ICBAL;
        char filename[33] = {0};
        for(int i = 0; i < 32; i++) { 
            uint8_t ch = atariRam[addr + i];
            if (ch == 155) break;
            filename[i] = ch;    
        } 
        fakeFile.open(filename);
        structLogs.opens.add(filename);
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 2) { // close
        pbiRequest->y = 1; 
        fakeFile.close();
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 3) { // get
        pbiRequest->y = 1; 
        int c = fakeFile.get();
        if (c < 0) 
            pbiRequest->y = 136;
        else
            pbiRequest->a = c; 
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 4) { // put
        pbiRequest->y = 1; 
        if (fakeFile.put(pbiRequest->a) < 0)
            pbiRequest->y = 136;
        pbiRequest->carry = 1; 
    } else if (pbiRequest->cmd == 5) { // status 
        pbiRequest->y = 1; 
        pbiRequest->carry = 0; // assume fail 
    } else if (pbiRequest->cmd == 6) { // special 
        pbiRequest->y = 1; 
        pbiRequest->carry = 0; // assume fail 
    } else if (pbiRequest->cmd == 7) { // low level io, see DCB
        pbiRequest->y = 1; 
        pbiRequest->carry = 0; // assume fail 
        AtariDCB *dcb = atariMem.dcb;
        uint16_t addrNO = (((uint16_t)dcb->DBUFHI) << 8) | dcb->DBUFLO;
        uint8_t *vaddr = banks[bankNr(addrNO) + BANKSEL_CPU + BANKSEL_RD] + (addrNO & bankOffsetMask);
        int sector = (((uint16_t)dcb->DAUX2) << 8) | dcb->DAUX1;
        structLogs.dcb.add(*dcb);
        if (0) { 
            SCOPED_INTERRUPT_ENABLE(pbiRequest);
            printf(DRAM_STR("DCB: "));
            StructLog<AtariDCB>::printEntry(*dcb);
            fflush(stdout);
            portDISABLE_INTERRUPTS();
        }
        if (dcb->DDEVIC == 0x31 && dcb->DUNIT >= 1 
            && dcb->DUNIT < sizeof(atariDisks)/sizeof(atariDisks[0]) + 1) {  // Device D1:
                DiskImage *disk = &atariDisks[dcb->DUNIT - 1]; 
            lastIoSec = elapsedSec;
            ioCount++;
            if (disk->valid()) {
                int sectorSize = disk->header.sectorSize;
                if (dcb->DCOMND == 0x53) { // SIO status command
                    // drive status https://www.atarimax.com/jindroush.atari.org/asio.html
                    vaddr[0] = (sectorSize != 128) ? 0x20 : 0x00; // bit 0 = frame err, 1 = cksum err, wr err, wr prot, motor on, sect size, unused, med density  
                    vaddr[1] = 0xff; // inverted bits: busy, DRQ, data lost, crc err, record not found, head loaded, write pro, not ready 
                    vaddr[2] = 0xff; // timeout for format 
                    vaddr[3] = 0xff; // copy of wd
                    dcb->DSTATS = 0x1;
                    pbiRequest->carry = 1;
                }
                int dbyt = (dcb->DBYTHI << 8) + dcb->DBYTLO;
                // first 3 sectors are always 128 bytes even on DD disks
                if (sector <= 3) sectorSize = 128;
                int offset = (sector - 1) * sectorSize;
                if (sector > 3 && sectorSize == 256) offset -= 3 * 128;
                
                if (dcb->DCOMND == 0x52 || dcb->DCOMND == 0xd2/*xdos sets 0x80?*/) {  // READ sector
                    SCOPED_INTERRUPT_ENABLE(pbiRequest);
                    disk->read(vaddr, offset, dbyt);
                    dcb->DSTATS = 0x1;
                    pbiRequest->carry = 1;
                }
                if (dcb->DCOMND == 0x50) {  // WRITE sector
                    SCOPED_INTERRUPT_ENABLE(pbiRequest);
                    disk->write(vaddr, offset, dbyt);
                    dcb->DSTATS = 0x1;
                    pbiRequest->carry = 1;
                }
                if (dcb->DCOMND == 0x3f) {  // get hi-speed capabilities
                    dcb->DSTATS = 0x1;
                    pbiRequest->carry = 1;
                    vaddr[0] = 0x28;
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
                    PercomBlock *percom = (PercomBlock *)vaddr;
                    int sectors = ((disk->header.pars + disk->header.parsHigh * 256) * 0x10) / disk->header.sectorSize;
                    percom->tracks = 1;
                    percom->stepRate = 3;
                    percom->secPerTrkHi = sectors >> 8;
                    percom->secPerTrkLo = sectors & 0xff;
                    percom->sides = 1;
                    percom->mfm = 4;
                    percom->bytesPerSectorHi = disk->header.sectorSize >> 8;
                    percom->bytesPerSectorLo = disk->header.sectorSize & 0xff;
                    percom->driveOnline = 0xff;
                    percom->unused[0] = percom->unused[1] = percom->unused[2] = 0;
                    dcb->DSTATS = 0x1;
                    pbiRequest->carry = 1;
                }
            }
        }
    } else if (pbiRequest->cmd == 8) { // IRQ
        clearInterrupt();
        SCOPED_INTERRUPT_ENABLE(pbiRequest);
        //sendHttpRequest();
        //connectToServer();
        pbiInterruptCount++;

    } else  if (pbiRequest->cmd == 10) { // wait for good vblank timing
        uint32_t vbTicks = 4005300;
        int offset = 3700000;
        //int offset = 0;
        int window = 1000;
        while( // Vblank synch is hard hmmm          
            ((XTHAL_GET_CCOUNT() - lastVblankTsc) % vbTicks) > offset + window
            ||   
            ((XTHAL_GET_CCOUNT() - lastVblankTsc) % vbTicks) < offset
        ) {}
    } else  if (pbiRequest->cmd == 11) { // wait for good vblank timing
        sysMonitorRequested = 0;
        sysMonitor.pbi(pbiRequest);
    } else  if (pbiRequest->cmd == 20) {
        SCOPED_INTERRUPT_ENABLE(); 
        //sendHttpRequest();
        //connectToServer();
        yield();
    }
}

void IRAM_ATTR handlePbiRequest(PbiIocb *pbiRequest) {  
    // Investigating halting the cpu instead of the stack-prog wait scheme
    // so far doens't work.
    
    // Assume pbi commands are always issued with the 6502 ready for a bus detach
    //pbiRequest->req = 0x2;

    if (needSafeWait(pbiRequest))
        return;

    halt6502();
    if (0) {
        SCOPED_INTERRUPT_ENABLE(pbiRequest);
        busyWait6502Ticks(20000);
    }
    //resume6502();
#if 0 
    while(bmonTail != bmonHead) { 
        uint32_t bmon = bmonArray[bmonTail];//REG_READ(SYSTEM_CORE_1_CONTROL_1_REG);
        bmonTail = (bmonTail + 1) & (bmonArraySz - 1); 
        uint32_t r0 = bmon >> bmonR0Shift;
        uint16_t addr = r0 >> addrShift;
        int rw = r0 & readWriteMask;
        int refresh = r0 & refreshMask;     
        if (refresh != 0 && (
            (addr & 0xff00) == 0xd500 || (rw == 0 && (addr == 0xd301 || addr == 0xd1ff)))) {
                SCOPED_INTERRUPT_ENABLE(pbiRequest);
                printf("caught addr %04d in bmon during pbiReq\n", (int)addr);
        }
    } 
#endif   
    pbiRequest->result = 0;
    handlePbiRequest2(pbiRequest);
    {
        DRAM_ATTR static int lastPrint = -999;
        if (elapsedSec - lastPrint >= 2) {
            SCOPED_INTERRUPT_ENABLE(pbiRequest);
            handleSerial();
            lastPrint = elapsedSec;
            static int lastIoCount = 0;
            printf(DRAM_STR("time %02d:%02d:%02d iocount: %8d (%3d) irqcount %d unmaps %d "
                "halts %d spur halts %d\n"), 
                elapsedSec/3600, (elapsedSec/60)%60, elapsedSec%60, ioCount,  
                ioCount - lastIoCount, 
                pbiInterruptCount, unmapCount, haltCount, spuriousHaltCount);
            fflush(stdout);
            lastIoCount = ioCount;
        }
        if (elapsedSec - lastScreenShot >= 90) {
            SCOPED_INTERRUPT_ENABLE(pbiRequest);
            handleSerial();
            dumpScreenToSerial('Y');
            fflush(stdout);
            lastScreenShot = elapsedSec;
        }
    } 
    if (pbiRequest->consol == 0 || pbiRequest->kbcode == 0xe5 || sysMonitorRequested) 
        pbiRequest->result |= 0x80;
    bmonTail = bmonHead;
    resume6502();
    busyWait6502Ticks(2);
    bmonTail = bmonHead;
    pbiRequest->req = 0;
    //atariRam[0x100 + pbiRequest->stackprog - 2] = 0;
}

void IRAM_ATTR handlePbiRequestOLD(PbiIocb *pbiRequest) {   
    pbiRequest->result = 0;
    handlePbiRequest2(pbiRequest);
    if ((pbiRequest->req & 0x2) != 0) {
        // Wait until we know the 6502 is safely in the stack-resident program. 
        // The instruction at stackprog + 4 is guaranteed to take enough ticks
        // for us to safely re-enable the bus without an interrupt occurring   
        uint16_t addr = 0;
        uint32_t refresh = 0;
        uint32_t startTsc = XTHAL_GET_CCOUNT();
        while(XTHAL_GET_CCOUNT() - startTsc < 240 * 100) {} // let core1 stabilize after interrupts and disruptions
        static const DRAM_ATTR int sprogTimeout = 240000000;
    #ifndef FAKE_CLOCK
        bmonTail = bmonHead;
        do {
            while(bmonHead == bmonTail) { 
                if (XTHAL_GET_CCOUNT() - startTsc > sprogTimeout) {
                    exitReason = sfmt("-3 stackprog timeout, stackprog 0x%02x", (int)pbiRequest->stackprog);
                    exitFlag = true;
                    return; // main loop will exit 
                }
            }
            uint32_t bmon = bmonArray[bmonTail];//REG_READ(SYSTEM_CORE_1_CONTROL_1_REG);
            bmonTail = (bmonTail + 1) & (bmonArraySz - 1); 
            uint32_t r0 = bmon >> bmonR0Shift;
            addr = r0 >> addrShift;
            refresh = r0 & refreshMask;     
        } while(refresh == 0 || addr != 0x100 + pbiRequest->stackprog - 2); // stackprog is only low-order byte
    #endif
        {
            DRAM_ATTR static int lastPrint = -999;
            if (elapsedSec - lastPrint >= 2) {
                SCOPED_INTERRUPT_ENABLE(pbiRequest);
                handleSerial();
                lastPrint = elapsedSec;
                static int lastIoCount = 0;
                printf(DRAM_STR("time %02d:%02d:%02d iocount: %8d (%3d) irqcount %d unmaps %d "
                    "halts %d spur halts %d\n"), 
                    elapsedSec/3600, (elapsedSec/60)%60, elapsedSec%60, ioCount,  
                    ioCount - lastIoCount, 
                    pbiInterruptCount, unmapCount, haltCount, spuriousHaltCount);
                fflush(stdout);
                lastIoCount = ioCount;
            }
            if (elapsedSec - lastScreenShot >= 90) {
                SCOPED_INTERRUPT_ENABLE(pbiRequest);
                handleSerial();
                dumpScreenToSerial('Y');
                fflush(stdout);
                lastScreenShot = elapsedSec;
            }
        } 
        if (pbiRequest->consol == 0 || pbiRequest->kbcode == 0xe5 || sysMonitorRequested) 
            pbiRequest->result |= 0x80;
        bmonTail = bmonHead;
        pbiRequest->req = 0;
        atariRam[0x100 + pbiRequest->stackprog - 2] = 0;
    } else { 
        if (pbiRequest->consol == 0 || pbiRequest->kbcode == 0xe5 || sysMonitorRequested) 
            pbiRequest->result |= 0x80;
        pbiRequest->req = 0;
    }
}

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
    if ((bmon & (refreshMask << bmonR0Shift)) == 0)
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
                    bmon = (bmon & bmonMask) | (0x80000000 | t.mark | busEnabledMark);
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

bool IRAM_ATTR bmonServiceQueue() {
    bool pbiReq = false;
    bool mmuChange = false;
    int bTail;
    do {
        while(bmonTail == bmonHead) {  
         //   ASM("nop;nop;nop;nop;nop;nop;nop;nop;nop;"); 
        }
        mmuChange = false;
        for(bTail = bmonTail; bTail != bmonHead; bTail = (bTail + 1) & (bmonArraySz - 1)) { 
            uint32_t r0 = bmonArray[bTail] >> bmonR0Shift;
            if ((r0 & readWriteMask) == 0) {
                uint32_t lastWrite = (r0 & addrMask) >> addrShift;
                if (lastWrite == 0xd301 || lastWrite == 0xd1ff) 
                    mmuChange = true;
                if (lastWrite == 0xd830 || lastWrite == 0xd840) 
                    pbiReq = true;
            }
        }
        if (mmuChange) 
            onMmuChange();
        for(bTail = bmonTail; bTail != bmonHead; bTail = (bTail + 1) & (bmonArraySz - 1)) { 
            // TODO: why does including this not let things even boot?
            bmonLog(bmonArray[bTail]); 
        }
        bmonTail = bTail;
    } while(mmuChange);
    return pbiReq;
}

void IRAM_ATTR core0LowPriorityTasks(); 
DRAM_ATTR int consecutiveBusIdle = 0;

void IRAM_ATTR core0Loop() { 
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

    uint32_t bmon = 0;
    bmonTail = bmonHead;
    while(1) {
        uint32_t stsc = XTHAL_GET_CCOUNT();
        const static DRAM_ATTR uint32_t bmonTimeout = 240 * 1000 * 10;
        const static DRAM_ATTR uint32_t bmonMask = 0x2fffffff;
        while(XTHAL_GET_CCOUNT() - stsc < bmonTimeout) {  
            // TODO: break this into a separate function, serviceBmonQueue(), maintain two pointers 
            // bTail1 and bTail2.   Loop bTail1 until queue is empty, call onMmuChange once if newport
            // or portb writes are observed, then increment bTail2 by one and process bmon logging,
            // then repeat.  Return only when both bTail1 and bTail2 == bmonHead, and after a cycle 
            // where no work has been done (ie: no onMmuChange, no bmon logging), ensuring the maximum
            // time is available for future work.   Return true if medium priority work was noticed, 
            // such as pbirequest.  Return false if no medium priority work was noted, indicating
            // low priority housekeeping routine should be called. 
            while(
                XTHAL_GET_CCOUNT() - stsc < bmonTimeout && 
                bmonHead == bmonTail) {
            }
            int bHead = bmonHead, bTail = bmonTail; // cache volatile values in local registers
            if (bHead == bTail)
	            continue;

            bmonMax = max((bHead - bTail) & (bmonArraySz - 1), bmonMax);
            bmon = bmonArray[bTail] & bmonMask;
            bmonTail = (bTail + 1) & (bmonArraySz - 1);
        
            uint32_t r0 = bmon >> bmonR0Shift;

            uint16_t addr = (r0 & addrMask) >> addrShift;
            if ((r0 & readWriteMask) == 0) {
                uint32_t lastWrite = addr;
                if (lastWrite == 0xd301) onMmuChange();
                if (lastWrite == 0xd1ff) onMmuChange();
                if ((lastWrite & 0xff00) == 0xd500 && atariCart.accessD500(lastWrite)) 
                    onMmuChange();
                // these banks have haltMask set in bankEnable and will halt the 6502 on any write.
                // restart the 6502 now that onMmuChange has had a chance to run. 
                if (bankNr(lastWrite) == bankNr(0xd500) 
                    || bankNr(lastWrite) == bankNr(0xd300)
                    || bankNr(lastWrite) == bankNr(0xd100)
                ) {
                    PROFILE_MMU((bmonHead - bmonTail) & (bmonArraySz - 1));
                    resume6502();
                }
                if (lastWrite == 0xd830) break;
                if (lastWrite == 0xd840) break;
                // && pbiROM[0x40] != 0) handlePbiRequest((PbiIocb *)&pbiROM[0x40]);
                //if (lastWrite == 0x0600) break;
            } else if ((r0 & refreshMask) != 0) {
                uint32_t lastRead = addr;
                //if ((lastRead & 0xff00) == 0xd500 && atariCart.accessD500(lastRead)) 
                //    onMmuChange();
                //if (bankNr(lastWrite) == bankNr(0xd500)) resume6502(); 
                //if (lastRead == 0xFFFA) lastVblankTsc = XTHAL_GET_CCOUNT();
            }    
            
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
            prerollBuffer[prerollIndex] = bmon;
            prerollIndex = (prerollIndex + 1) & (prerollBufferSize - 1); 
        }

        //restartHalted6502();

        // The above loop exits to here every 10ms or when an interesting address has been read 
        PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
        if (pbiRequest[0].req != 0) { 
            handlePbiRequest(&pbiRequest[0]); 
        } else if (pbiRequest[1].req != 0) { 
            handlePbiRequest(&pbiRequest[1]);
        }

        if(0) {
            // We're missing some halts in the bmon queue, which makes sense. 
            // TODO: a more effecient way of detecting a halted 6502, or somehow 
            // ensure we don't miss ANY bmon traffic. 
            uint32_t stsc = XTHAL_GET_CCOUNT();
            pinDisableMask |= haltMask;
            int bHead = bmonHead;
            while(
                XTHAL_GET_CCOUNT() - stsc < bmonTimeout && 
                bmonHead == bHead) {
            }
            bHead = bmonHead;
            while(
                XTHAL_GET_CCOUNT() - stsc < bmonTimeout && 
                bmonHead == bHead) {
            }
            pinDisableMask &= (~haltMask);
        }

        static uint8_t lastNewport = 0;
        if (D000Write[0x1ff] != lastNewport) { 
            lastNewport = D000Write[0x1ff];
            onMmuChange();
        }
#if 0 
        static uint8_t lastPortb = 0;
        if (D000Write[0x301] != lastPortb) { 
            lastPortb = D000Write[0x301];
            onMmuChange();
        }
#endif
        if (deferredInterrupt 
            && (D000Write[0x1ff] & pbiDeviceNumMask) != pbiDeviceNumMask
            && (D000Write[0x301] & 0x1) != 0
        )
            raiseInterrupt();

#if 1//bankSize <= 0x100 // we don't have a way to handle reads to 0xd1ff with large bank sizes yet 
        if (/*XXINT*/1 && (elapsedSec > 30 || ioCount > 1000)) {
            static uint32_t ltsc = 0;
            static const DRAM_ATTR int isrTicks = 240 * 1000 * 100; // 10Hz
            if (XTHAL_GET_CCOUNT() - ltsc > isrTicks) { 
                ltsc = XTHAL_GET_CCOUNT();
                raiseInterrupt();
            }
        }
#endif

#if defined(FAKE_CLOCK) || defined (RAM_TEST)
        if (1 && elapsedSec > 30) { //XXFAKEIO
            // Stuff some fake PBI commands to exercise code in the core0 loop during timing tests 
            static uint32_t lastTsc = XTHAL_GET_CCOUNT();
            static const DRAM_ATTR uint32_t tickInterval = 240 * 1000;
            if (XTHAL_GET_CCOUNT() - lastTsc > tickInterval) {
                lastTsc = XTHAL_GET_CCOUNT();
                PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
                static int step = 0;
                if (step == 0) { 
                    // stuff a fake CIO put request
                    pbiRequest->cmd = 20; // put 
                    pbiRequest->a = ' ';
                    pbiRequest->req = 2;
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
                    pbiRequest->cmd = 7; // read a sector 
                    pbiRequest->req = 2;
                } else if (step == 2) { 
                    
                }
                step = (step + 1) % 2;
            }
        }
#endif 

        static const DRAM_ATTR int keyTicks = 150 * 240 * 1000; // 150ms
        EVERYN_TICKS(keyTicks) { 
            if (simulatedKeyInput.available()) { 
                uint8_t c = simulatedKeyInput.getKey();
                if (c != 255) 
                    atariRam[764] = ascii2keypress[c];
                bmonMax = 0;
            }
        }

        EVERYN_TICKS(240 * 1000000) { // XXSECOND
            elapsedSec++;

            if (elapsedSec == 15 && ioCount > 0) {
                //memcpy(&atariRam[0x0600], page6Prog, sizeof(page6Prog));
                //simulatedKeyInput.putKeys(DRAM_STR("CAR\233\233PAUSE 1\233\233\233E.\"J:X\"\233"));
                //simulatedKeyInput.putKeys("    \233DOS\233  \233DIR D2:\233");
#ifdef BOOT_SDX
                simulatedKeyInput.putKeys(DRAM_STR("-2:X\233"));
#else
                simulatedKeyInput.putKeys(DRAM_STR("CAR\233  PAUSE 1\233E.\"J:X\"\233"));

#endif
            }
            if (0 && (elapsedSec % 10) == 0) {  // XXSYSMON
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
            if (sizeof(bmonTriggers) >= sizeof(BmonTrigger) && secondsWithoutWD == wdTimeout - 1) {
                bmonTriggers[0].value = bmonTriggers[0].mask = 0;
                bmonTriggers[0].depth = 3000;
                bmonTriggers[0].count = 1;
        
            }
#endif
            if (wdTimeout > 0 && secondsWithoutWD >= wdTimeout) { 
                exitReason = "-1 Watchdog timeout";
                break;
            }
            if (ioTimeout > 0 && elapsedSec - lastIoSec > ioTimeout) { 
                exitReason = "-2 IO timeout";
                break;
            }
            
#endif
            if (elapsedSec == 1) { 
                bmonMax = mmuChangeBmonMaxEnd = mmuChangeBmonMaxStart = 0;
                for(int i = 0; i < numProfilers; i++) profilers[i].clear();
            }
#if 0 // XXPOSTDUMP
            if (sizeof(bmonTriggers) >= sizeof(BmonTrigger) && elapsedSec == opt.histRunSec - 1) {
                bmonTriggers[0].value = bmonTriggers[0].mask = 0;
                bmonTriggers[0].depth = 1000;
                bmonTriggers[0].count = 1;
            }
#endif

            if(elapsedSec > opt.histRunSec && opt.histRunSec > 0) {
                exitReason = "0 Specified run time reached";   
                break;
            }
            if(atariRam[754] == 0xef || atariRam[764] == 0xef) {
                exitReason = "1 Exit hotkey pressed";
                break;
            }
            if(atariRam[754] == 0xee || atariRam[764] == 0xee) {
                wdTimeout = ioTimeout = 120;
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

#if 0
void IRAM_ATTR core0LoopNEW2() { 
#ifdef RAM_TEST
    // disable PBI ROM by corrupting it 
    pbiROM[0x03] = 0xff;
#endif
    //uint32_t lastBmon = 0;
    int bmonCaptureDepth = 0;
    psramPtr = psram;

    const static DRAM_ATTR int prerollBufferSize = 64; // must be power of 2
    uint32_t prerollBuffer[prerollBufferSize]; 
    uint32_t prerollIndex = 0;

    if (psram == NULL) {
        for(auto &t : bmonTriggers) t.count = 0;
    }

    uint32_t bmon = 0;
    bmonTail = bmonHead;
    while(!exitFlag) {
        uint32_t stsc = XTHAL_GET_CCOUNT();
        const static DRAM_ATTR uint32_t bmonTimeout = 240 * 1000 * 10;
        const static DRAM_ATTR uint32_t bmonMask = 0x2fffffff;
        while(XTHAL_GET_CCOUNT() - stsc < bmonTimeout) {  
            while(
                XTHAL_GET_CCOUNT() - stsc < bmonTimeout && 
                bmonHead == bmonTail) {
            }
            int bHead = bmonHead, bTail = bmonTail; // cache volatile values in local registers
            if (bHead == bTail)
	            continue;

            bmon = bmonArray[bTail] & bmonMask;
            bmonTail = (bTail + 1) & (bmonArraySz - 1);
        
            uint32_t r0 = bmon >> bmonR0Shift;

            if ((r0 & readWriteMask) == 0) {
                uint32_t lastWrite = (r0 & addrMask) >> addrShift;
                if (lastWrite == 0xd301) onMmuChange();
                else if (lastWrite == 0xd1ff) onMmuChange();
                else if (lastWrite == 0xd830) break;
                else if (lastWrite == 0xd840) break;
            } else {
                //uint32_t lastRead = (r0 & addrMask) >> addrShift;
                //if (lastRead == 0xFFFA) lastVblankTsc = XTHAL_GET_CCOUNT();
            }    
            bmonLog(bmon);
        }

        // The above loop exits to here every 10ms or when an interesting address has been read 
        core0LowPriorityTasks();
    }
}

void IRAM_ATTR core0LoopNEW() { 
#ifdef RAM_TEST
    // disable PBI ROM by corrupting it 
    pbiROM[0x03] = 0xff;
#endif
    psramPtr = psram;
    if (psramPtr == NULL) {
        for(auto &t : bmonTriggers) t.count = 0;
    }        
    uint32_t lastLowPri = XTHAL_GET_CCOUNT();
    
    while(!exitFlag) {
        if (bmonServiceQueue()) { 
            PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
            if (pbiRequest[0].req != 0) {
                handlePbiRequest(&pbiRequest[0]); 
            } else if (pbiRequest[1].req != 0) { 
                handlePbiRequest(&pbiRequest[1]);
            }
        } else if (XTHAL_GET_CCOUNT() - lastLowPri > 240 * 1000 * 10 /*10ms*/) {
            core0LowPriorityTasks();
            lastLowPri = XTHAL_GET_CCOUNT();
        }
    }
    if (exitReason.length() == 0) 
        exitReason = "2 Exit command received";
}

inline IRAM_ATTR void core0LowPriorityTasks() { 
        static uint8_t lastNewport = 0;
        if (D000Write[0x1ff] != lastNewport) { 
            lastNewport = D000Write[0x1ff];
            onMmuChange();
        }
        static uint8_t lastPortb = 0;
        if (D000Write[0x301] != lastPortb) { 
            lastPortb = D000Write[0x301];
            onMmuChange();
        }
        if (deferredInterrupt 
            && (D000Write[0x1ff] & pbiDeviceNumMask) != pbiDeviceNumMask
            && (D000Write[0x301] & 0x1) != 0
        ) {
            raiseInterrupt();
        }
        if (/*XXINT*/0 && (elapsedSec > 30 || ioCount > 1000)) {
            static uint32_t ltsc = 0;
            static const DRAM_ATTR int isrTicks = 240 * 1000 * 100; // 10Hz
            if (XTHAL_GET_CCOUNT() - ltsc > isrTicks) { 
                ltsc = XTHAL_GET_CCOUNT();
                raiseInterrupt();
            }
        }

        if (0) { // XXMEMTEST
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
                    ioCount++; 
            }
        }

#if defined(FAKE_CLOCK) || defined (RAM_TEST)
        if (1 && elapsedSec > 10) { //XXFAKEIO
            // Stuff some fake PBI commands to exercise code in the core0 loop during timing tests 
            static uint32_t lastTsc = XTHAL_GET_CCOUNT();
            static const DRAM_ATTR uint32_t tickInterval = 240 * 1000;
            if (XTHAL_GET_CCOUNT() - lastTsc > tickInterval) {
                lastTsc = XTHAL_GET_CCOUNT();
                PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
                static int step = 0;
                if (step == 0) { 
                    // stuff a fake CIO put request
                    pbiRequest->cmd = 4; // put 
                    pbiRequest->a = ' ';
                    pbiRequest->req = 1;
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
                    pbiRequest->cmd = 7; // read a sector 
                    pbiRequest->req = 2;
                } else if (step == 2) { 
                    
                }
                step = (step + 1) % 2;
            }
        }
#endif 

        { // TODO:  EVERYN_TICKS macro broken, needs its own scope. 
            static const DRAM_ATTR int keyTicks = 150 * 240 * 1000; // 150ms
            EVERYN_TICKS(keyTicks) { 
                if (simulatedKeyInput.available()) { 
                    uint8_t c = simulatedKeyInput.getKey();
                    if (c != 255) 
                        atariRam[764] = ascii2keypress[c];
                    bmonMax = 0;
                }
            }
        }
        if (1) {  
            PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
            if (pbiRequest[0].req != 0) { 
                handlePbiRequest(&pbiRequest[0]); 
            } else if (pbiRequest[1].req != 0) { 
                handlePbiRequest(&pbiRequest[1]);
            }
        }
        EVERYN_TICKS(240 * 1000000) { // XXSECOND
            elapsedSec++;
#if 0
            if (elapsedSec == 8 && ioCount == 0) {
                memcpy(&atariRam[0x0600], page6Prog, sizeof(page6Prog));
                addSimKeypress("A=USR(1546)\233");
            }
#endif

            if (elapsedSec == 10 && ioCount > 0) {
                //memcpy(&atariRam[0x0600], page6Prog, sizeof(page6Prog));
                //simulatedKeyInput.putKeys(DRAM_STR("CAR\233\233PAUSE 1\233\233\233E.\"J:X\"\233"));
                //simulatedKeyInput.putKeys("    \233DOS\233     \233DIR D2:\233");
                simulatedKeyInput.putKeys(DRAM_STR("CAR \233PAUSE 1\233E.\"J\233"));
            }
            if (1 && (elapsedSec % 10) == 0) {  // XXSYSMON
                sysMonitorRequested = 1;
            }

#ifndef FAKE_CLOCK
            if (1) { 
                DRAM_ATTR static int lastWD = 0;
                if (1) { 
                    if (watchDogCount == lastWD) { 
                        secondsWithoutWD++;
                    } else { 
                        secondsWithoutWD = 0;
                    }
                } else { 
                    if (atariRam[1537] == 0) { 
                        secondsWithoutWD++;
                    }
                    atariRam[1537] = 0;
                }

                lastWD = watchDogCount;
#if 0 // XXPOSTDUMP
                if (sizeof(bmonTriggers) >= sizeof(BmonTrigger) && secondsWithoutWD == wdTimeout - 1) {
                    bmonTriggers[0].value = bmonTriggers[0].mask = 0;
                    bmonTriggers[0].depth = 3000;
                    bmonTriggers[0].count = 1;
		   
                }
#endif
                if (wdTimeout > 0 && secondsWithoutWD >= wdTimeout) { 
                    exitReason = "-1 Watchdog timeout";
                    exitFlag = true;
                }
                if (ioTimeout > 0 && lastIoSec - elapsedSec > ioTimeout) { 
                    exitReason = "-2 IO timeout";
                    exitFlag = true;
                }
            }
#endif

            if (elapsedSec == 1) { 
                bmonMax = mmuChangeBmonMaxEnd = mmuChangeBmonMaxStart = 0;
            }
            if (elapsedSec == 1) { 
               for(int i = 0; i < numProfilers; i++) profilers[i].clear();
            }
#if 0 // XXPOSTDUMP
            if (sizeof(bmonTriggers) >= sizeof(BmonTrigger) && elapsedSec == opt.histRunSec - 1) {
                bmonTriggers[0].value = bmonTriggers[0].mask = 0;
                bmonTriggers[0].depth = 1000;
                bmonTriggers[0].count = 1;
            }
#endif

            if(elapsedSec > opt.histRunSec && opt.histRunSec > 0) {
                exitReason = "0 Specified run time reached";   
                exitFlag = true;
            }
            if(atariRam[754] == 0xef || atariRam[764] == 0xef) {
                exitReason = "1 Exit hotkey pressed";
                exitFlag = true;
            }
            if(atariRam[754] == 0xee || atariRam[764] == 0xee) {
                wdTimeout = 120;
                secondsWithoutWD = 0;
                atariRam[712] = 255;
            }
        }
    
}
#endif //#if0 
void IFLASH_ATTR threadFunc(void *) { 
    printf("CORE0: threadFunc() start\n");
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);

#ifdef BUS_DETACH
    printf("BUS_DETACH is set\n");
#else
    printf("BUS_DETACH is NOT set\n");
#endif
    printf("opt.fakeClock %d opt.histRunSec %.2f\n", opt.fakeClock, opt.histRunSec);
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
    busywait(.5);
    disableBus();

    busywait(.001);
    REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
    busywait(.001);
    enableCore0WDT();
    portENABLE_INTERRUPTS();
    //_xt_intexc_hooks[XCHAL_NMILEVEL] = oldnmi;
    //__asm__("wsr %0,PS" : : "r"(oldint));

#ifndef FAKE_CLOCK
    printf("bmonMax: %d mmuChangeBmonMaxEnd: %d mmuChangeBmonMaxStart: %d\n", bmonMax, mmuChangeBmonMaxEnd, mmuChangeBmonMaxStart);   
    printf("bmonArray:\n");
    for(int i = 0; i < bmonArraySz; i++) { 
        uint32_t r0 = (bmonCopy[i] >> 8);
        uint16_t addr = r0 >> addrShift;
        char rw = (r0 & readWriteMask) != 0 ? 'R' : 'W';
        if ((r0 & refreshMask) == 0) rw = 'F';
        uint8_t data = (bmonCopy[i] & 0xff);
        printf("%c %04x %02x\n", rw, addr, data);
    }
#endif


    uint64_t totalEvents = 0;
    for(int i = 0; i < profilers[1].maxBucket; i++) {
        totalEvents += profilers[1].buckets[i];
    }
    for(int i = 0; i < profilers[2].maxBucket; i++) {
        totalEvents += profilers[2].buckets[i];
    }
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
            //if ((*p & copyResetMask) && !(*p &casInh_Mask))
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
                uint16_t addr = r0 >> addrShift;
                char rw = (r0 & readWriteMask) != 0 ? 'R' : 'W';
                uint8_t data = (*p & 0xff);
                const char *op = ops[data];
                if (op == NULL) op = "";
                if (*p != 0) 
                    printf("%c %04x %02x   %s\n", rw, addr, data, op); 
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
    printf("reg[0xd301] = 0x%02x\n", D000Write[0x301]);
    printf("ioCount %d, interruptCount %d\n", ioCount, pbiInterruptCount);
    structLogs.print();
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
    printf("\n");

    dumpScreenToSerial('B');
#endif

    printf("\n0xd1ff: %02x\n", atariRam[0xd1ff]);
    printf("0xd830: %02x\n", atariRam[0xd830]);
    
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    int memReadErrors = (atariRam[0x609] << 24) + (atariRam[0x608] << 16) + (atariRam[0x607] << 16) + atariRam[0x606];
    printf("SUMMARY %-10.2f/%.0f e%d i%d d%d %s\n", millis()/1000.0, opt.histRunSec, memReadErrors, 
    pbiInterruptCount, ioCount, exitReason.c_str());
    printf("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    printf("GPIO_EN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_ENABLE_REG),REG_READ(GPIO_ENABLE1_REG)); 

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
        app_cpu_stack_ptr = heap_caps_malloc(1024, MALLOC_CAP_DMA);
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
    delay(500);
    printf("setup()\n");
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
        for(auto p : pins) pinMode(p, INPUT_PULLDOWN);
        pinDisable(casInh_pin);

        while(1) { 
            for(auto p : pins) {
                printf("%02d:%d ", p, digitalRead(p));
            }
            printf("\n");
            delay(50);
        }
    }

    for(auto i : pins) pinMode(i, INPUT);

    usb_serial_jtag_driver_config_t jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&jtag_config);

    if (opt.testPins) { 
        for(auto p : pins) pinMode(p, INPUT_PULLUP);
        while(1) { 
            for(auto p : pins) {
                printf("%02d:%d ", p, digitalRead(p));
            }
            printf("\n");
            delay(200);
        }
    }

    //gpio_dump_io_configuration(stdout, (1ULL << 19) | (1ULL << 20) | (1));

    for(int i = 0; i < 16; i++) {
        xeBankMem[i] = &atariRam[0x4000];
    }
#ifdef XE_BANK
#ifdef RAMBO_XL256
    for(int i = 0; i < 4; i++) {
        xeBankMem[i] = &atariRam[0x4000];
    }
    for(int i = 4; i < 16; i++) {
        xeBankMem[i] = (uint8_t *)heap_caps_malloc(16 * 1024, MALLOC_CAP_INTERNAL);
        while (xeBankMem[i] == NULL) { 
            printf("malloc(16K) failed xeBankMem %d\n", i);
            heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
            delay(1000);
        }
        bzero(xeBankMem[i], 16 * 1024);
    }
#else // Standard XE 64K banked men 
    for(int i = 0; i < 4; i++) {
        uint8_t *mem = (uint8_t *)heap_caps_malloc(16 * 1024, MALLOC_CAP_INTERNAL);
        xeBankMem[i + 0b0000] = mem;
        xeBankMem[i + 0b0100] = mem;
        xeBankMem[i + 0b1000] = mem;
        xeBankMem[i + 0b1100] = mem;
        while (mem == NULL) { 
            printf("malloc(16K) failed xeBankMem %d\n", i);
            heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
            delay(1000);
        }
        bzero(mem, 16 * 1024);
    }
#if 1
    // Experimenting trying to add a couple more banks of ram where SDX will find it 
    // This should look like the Compy Shop 192K bank selection portb bits 2,3,6 
    for(int i = 0; i < 4; i++) {
        uint8_t *mem = (uint8_t *)heap_caps_malloc(16 * 1024, MALLOC_CAP_INTERNAL);
        while (mem == NULL) { 
            printf("malloc(16K) failed xeBankMem %d\n", i);
            heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
            delay(1000);
        }
        xeBankMem[i + 0b0100] = mem;
        xeBankMem[i + 0b0000] = mem;
        bzero(mem, 16 * 1024);
    }
#endif // #if 0 
#endif
#endif

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

    //atariDisks[0].open("sd43g.720k.atr", true);
#ifdef BOOT_SDX
    atariDisks[0].open("/toolkit.atr", true);
    atariCart.open("/SDX450_maxflash1.car");
#else
    atariDisks[0].open("/d1.atr", true);
    //atariCart.open("/SDX450_maxflash1.car");
#endif
    atariDisks[1].open("/d2.atr", true);

    //atariCart.open("Joust.rom");
    //atariCart.open("Edass.car");
    //atariCart.open("SDX450_maxflash1.car");

#if 0
    // 169572 before sdkconf changes
    // 174595 after malloc and malloc 0 changes
    // 91719 with connectWiFi 
    // 92207 after lwip and wifi changes

    connectWifi(); // 82876 bytes 
    //connectToServer();
    //start_webserver();  //12516 bytes 
#endif

    while(0) { 
        yield();
        delay(500);
        printf("OK\n");
    }
    for(auto i : pins) pinMode(i, INPUT);
    while(opt.watchPins) { 
            delay(100);
            printf("PU   %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG));
    }

    if (opt.fakeClock) { // simulate clock signal 
        pinMode(clockPin, OUTPUT);
        digitalWrite(clockPin, 0);
        ledcAttachChannel(clockPin, testFreq, 1, 0);
#ifdef ARDUINO
        ledcWrite(clockPin, 1);
#else
        ledcWrite(0, 1);
#endif

        pinMode(readWritePin, OUTPUT);
        digitalWrite(readWritePin, 0);
        ledcAttachChannel(readWritePin, testFreq / 8, 1, 2);
#ifdef ARDUINO
        ledcWrite(readWritePin, 1);
#else
        ledcWrite(2, 1);
#endif

        pinMode(casInh_pin, OUTPUT);
        digitalWrite(casInh_pin, 1);
        ledcAttachChannel(casInh_pin, testFreq / 2, 1, 4);
        ledcWrite(4, 1);

        // write 0xd1ff to address pins to simulate worst-case slowest address decode
        for(int bit = 0; bit < 16; bit ++)  
            pinMode(addr0Pin + bit, ((0xd1ff >> bit) & 1) == 1 ? INPUT_PULLUP : INPUT_PULLDOWN);

        //gpio_set_drive_capability((gpio_num_t)clockPin, GPIO_DRIVE_CAP_MAX);
        pinMode(mpdPin, INPUT_PULLDOWN);
        pinMode(refreshPin, INPUT_PULLUP);
        //pinMode(casInh_pin, INPUT_PULLUP);
        pinMode(extSel_Pin, INPUT_PULLUP);
    }

    pinDisable(casInh_pin);
    for(int i = 0; i < 1; i++) { 
        printf("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    }

    printf("freq %.4fMhz threshold %d halfcycle %d psram %p\n", 
        testFreq / 1000000.0, lateThresholdTicks, (int)halfCycleTicks, psram);

    gpio_matrix_in(clockPin, CORE1_GPIO_IN0_IDX, false);
    digitalWrite(interruptPin, 1);
    pinMode(interruptPin, OUTPUT_OPEN_DRAIN);
    digitalWrite(interruptPin, 1);
    //gpio_matrix_out(interruptPin, CORE1_GPIO_OUT0_IDX, false, false);
    pinMode(interruptPin, OUTPUT_OPEN_DRAIN);
    pinMode(haltPin, OUTPUT_OPEN_DRAIN);
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, interruptMask);
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, haltMask);
    digitalWrite(interruptPin, 0);
    digitalWrite(haltPin, 0);
    for(int i = 0; i < 8; i++) { 
        pinMode(data0Pin + i, OUTPUT); // TODO: Investigate OUTPUT_OPEN_DRAIN doesn't work, would enable larger page sizes if it did 
    }
    clearInterrupt();
    memoryMapInit();
    enableBus();
    startCpu1();
    busywait(.001);
    //threadFunc(NULL);
    xTaskCreatePinnedToCore(threadFunc, "th", 8 * 1024, NULL, 0, NULL, 0);
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

// need PBI lines casInh_, WRT, phi2, ADDR0-15, DATA0-7, EXTSEL

// NOTES:
// 8-pin i2c io expander: https://media.digikey.com/pdf/Data%20Sheets/NXP%20PDFs/PCF8574(A).pdf
// TODO: verify polarity of RW, MPD, casInh, etc 


std::string vsfmt(const char *format, va_list args) {
        va_list args2;
        va_copy(args2, args);
        static DRAM_ATTR char buf[64]; // don't understand why stack variable+copy is faster
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
