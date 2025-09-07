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
#include "pinDefs.h"
#include "core1.h" 
#ifndef ARDUINO
#include "arduinoLite.h"
#endif

#include "asmdefs.h"
#include "extMem.h"

#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#include "diskImage.h"
#include "diskFlash.h"
#include "diskSmb.h"

void IRAM_ATTR NEWneopixelWrite(uint8_t red_val, uint8_t green_val, uint8_t blue_val) {
    //busyWaitCCount(100);
    //return;       
    uint32_t stsc;
    int color[3] = {green_val, red_val, blue_val};
    int longCycles = 175;
    int shortCycles = 90;
    int i = 0;
    for (int col = 0; col < 3; col++) {
        for (int bit = 0; bit < 8; bit++) {
            if ((color[col] & (1 << (7 - bit)))) {
                dedic_gpio_cpu_ll_write_all(1);
                stsc = XTHAL_GET_CCOUNT();
                while(XTHAL_GET_CCOUNT() - stsc < longCycles) {}

                dedic_gpio_cpu_ll_write_all(0);
                stsc = XTHAL_GET_CCOUNT();
                while(XTHAL_GET_CCOUNT() - stsc < shortCycles) {}
            } else {
                // LOW bit
                dedic_gpio_cpu_ll_write_all(1);
                stsc = XTHAL_GET_CCOUNT();
                while(XTHAL_GET_CCOUNT() - stsc < shortCycles) {}

                dedic_gpio_cpu_ll_write_all(0);
                stsc = XTHAL_GET_CCOUNT();
                while(XTHAL_GET_CCOUNT() - stsc < longCycles) {}
            }
            i++;
        }
    }
}


void sendHttpRequest();
void connectWifi();
void connectToServer();
void start_webserver(void);

// boot SDX cartridge image - not working well enough to base stress tests on it 
//#define BOOT_SDX

#ifndef BOOT_SDX
#define RAMBO_XL256
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

#if 0 // TMP: investigate removing these, should be unneccessary due to linker script
#undef DRAM_ATTR
#define DRAM_ATTR
#undef IRAM_ATTR
#define IRAM_ATTR 
#endif 

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

DRAM_ATTR RAM_VOLATILE uint8_t *pages[nrPages * 4];
DRAM_ATTR uint32_t pageEnable[nrPages * 4];
DRAM_ATTR RAM_VOLATILE uint8_t *baseMemPages[nrPages] = {0};

//DRAM_ATTR uint8_t *xeBankMem[16] = {0};
DRAM_ATTR RAM_VOLATILE uint8_t atariRam[baseMemSz] = {0x0};
DRAM_ATTR RAM_VOLATILE uint8_t dummyRam[pageSize] = {0x0};
DRAM_ATTR RAM_VOLATILE uint8_t d000Write[0x800] = {0x0};
DRAM_ATTR RAM_VOLATILE uint8_t d000Read[0x800] = {0xff};
DRAM_ATTR RAM_VOLATILE uint8_t d000BaseMem[0x800] = {0};
DRAM_ATTR RAM_VOLATILE uint8_t extMemWindow[0x4000] = {0x0};
DRAM_ATTR RAM_VOLATILE uint8_t screenMem[(pageNr(40 * 24) + 1) * pageSize];
DRAM_ATTR RAM_VOLATILE uint8_t pbiROM[0x800] = {
#include "pbirom.h"
};
//DRAM_ATTR RAM_VOLATILE uint8_t screenRam[pageSize * 5] = {0};
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


DRAM_ATTR ExtBankPool extMem; 

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
        .mask = (bus.refresh_.mask) << bmonR0Shift,                            // ignore refresh bus traffic  
        .value = (0) << bmonR0Shift,
    },
    {
        .mask = (bus.rw.mask | (0xf400 << bus.addr.shift)) << bmonR0Shift,   // ignore reads from char map  
        .value = (bus.rw.mask | (0xe000 << bus.addr.shift)) << bmonR0Shift,
    },
//    {
//        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift,   // ignore reads from 0x00ff
//        .value = (pins.read.mask | (0x00ff << pins.addr.shift)) << bmonR0Shift,
//    },
    {
        .mask = (bus.rw.mask | (0xf800 << bus.addr.shift)) << bmonR0Shift,   // ignore reads from screen mem
        .value = (bus.rw.mask | (0xb800 << bus.addr.shift)) << bmonR0Shift,
    },
#endif
};

//DRAM_ATTR volatile vector<BmonTrigger> bmonTriggers = {
DRAM_ATTR BmonTrigger bmonTriggers[] = {/// XXTRIG 
#if 0 //TODO why does this trash the bmon timings?
    { 
        .mask =  (((0 ? pins.read.mask : 0) | (0xff00 << pins.addr.shift)) << bmonR0Shift) | (0x00), 
        .value = (((0 ? pins.read.mask : 0) | (0xd500 << pins.addr.shift)) << bmonR0Shift) | (0x00),
        .mark = 0,
        .depth = 10,
        .preroll = 5,
        .count = INT_MAX,
        .skip = 0 // TODO - doesn't work? 
    },
#endif
#if 0 
    { 
        .mask =  (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (0             | (0xd301 << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 2,
        .preroll = 0,
        .count = INT_MAX,
        .skip = 0 // TODO - doesn't work? 
    },
#endif
#if 0 // TODO: too many bmonTriggers slows down IO and hangs the system
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (0xfffa << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (0xfffe << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (39968 << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (0xfffa << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (0xfffe << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
    { /// XXTRIG
        .mask = (pins.read.mask | (0xffff << pins.addr.shift)) << bmonR0Shift, 
        .value = (pins.read.mask | (39968 << pins.addr.shift)) << bmonR0Shift,
        .mark = 0,
        .depth = 0,
        .preroll = 0,
        .count = 1000000,
        .skip = 0 // TODO - doesn't work? 
    },
#endif
};
DRAM_ATTR BUSCTL_VOLATILE uint32_t pinReleaseMask = bus.irq_.mask | bus.data.mask | bus.extSel.mask | bus.mpd.mask;
DRAM_ATTR BUSCTL_VOLATILE uint32_t pinEnableMask = ~0;

DRAM_ATTR uint32_t busEnabledMark;
DRAM_ATTR BUSCTL_VOLATILE uint32_t pinDriveMask = 0;
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

inline IRAM_ATTR void mmuAddBaseRam(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int b = pageNr(start); b <= pageNr(end); b++)  
        baseMemPages[b] = (mem == NULL) ? NULL : mem + ((b - pageNr(start)) * pageSize);
}

inline IRAM_ATTR void mmuMapRangeRW(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int b = pageNr(start); b <= pageNr(end); b++) { 
        pages[b + PAGESEL_WR + PAGESEL_CPU] = mem + (b - pageNr(start)) * pageSize;
        pages[b + PAGESEL_RD + PAGESEL_CPU] = mem + (b - pageNr(start)) * pageSize;
        pageEnable[b + PAGESEL_CPU + PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
        pageEnable[b + PAGESEL_CPU + PAGESEL_WR] = 0;
    }
}

inline IRAM_ATTR void mmuMapRangeRWIsolated(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int b = pageNr(start); b <= pageNr(end); b++) { 
        pages[b + PAGESEL_WR + PAGESEL_CPU] = mem + (b - pageNr(start)) * pageSize;
        pages[b + PAGESEL_RD + PAGESEL_CPU] = mem + (b - pageNr(start)) * pageSize;
        pageEnable[b + PAGESEL_CPU + PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
        pageEnable[b + PAGESEL_CPU + PAGESEL_WR] = bus.extSel.mask;
    }
}

inline IRAM_ATTR void mmuMapRangeRO(uint16_t start, uint16_t end, uint8_t *mem) { 
    for(int b = pageNr(start); b <= pageNr(end); b++) { 
        pages[b + PAGESEL_WR + PAGESEL_CPU] = &dummyRam[0];
        pages[b + PAGESEL_RD + PAGESEL_CPU] = mem + (b - pageNr(start)) * pageSize;
        pageEnable[b + PAGESEL_CPU + PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
        pageEnable[b + PAGESEL_CPU + PAGESEL_WR] = bus.extSel.mask;
    }
}

inline IRAM_ATTR void mmuUnmapRange(uint16_t start, uint16_t end) { 
    for(int b = pageNr(start); b <= pageNr(end); b++) { 
        pages[b + PAGESEL_WR + PAGESEL_CPU] = &dummyRam[0];
        pages[b + PAGESEL_RD + PAGESEL_CPU] = &dummyRam[0];
        pageEnable[b + PAGESEL_CPU + PAGESEL_RD] = 0;
        pageEnable[b + PAGESEL_CPU + PAGESEL_WR] = 0;
    }
}

inline IRAM_ATTR void mmuRemapBaseRam(uint16_t start, uint16_t end) {
    for(int b = pageNr(start); b <= pageNr(end); b++) { 
        if (baseMemPages[b] != NULL) { 
            pages[b + PAGESEL_WR + PAGESEL_CPU] = baseMemPages[b];
            pages[b + PAGESEL_RD + PAGESEL_CPU] = baseMemPages[b];
            pageEnable[b + PAGESEL_CPU + PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
            pageEnable[b + PAGESEL_CPU + PAGESEL_WR] = 0;
        } else { 
            pages[b + PAGESEL_WR + PAGESEL_CPU] = &dummyRam[0];
            pages[b + PAGESEL_RD + PAGESEL_CPU] = &dummyRam[0];
            pageEnable[b + PAGESEL_CPU + PAGESEL_RD] = 0;
            pageEnable[b + PAGESEL_CPU + PAGESEL_WR] = 0;
        }
    }
}

inline IRAM_ATTR void mmuMapPbiRom(bool pbiEn, bool osEn) {
    if (pbiEn) {
        mmuMapRangeRWIsolated(_0xd800, _0xdfff, &pbiROM[0]);
    } else if(osEn) {
        mmuUnmapRange(_0xd800, _0xdfff);
    } else {
        mmuRemapBaseRam(_0xd800, _0xdfff);
    }
    if (pbiEn) { 
        pinReleaseMask &= (~bus.mpd.mask);
        pinDriveMask |= bus.mpd.mask;
    } else { 
        pinReleaseMask |= bus.mpd.mask;
        pinDriveMask &= (~bus.mpd.mask);
    }
}

// Called any time values in portb(0xd301) or newport(0xd1ff) change
IRAM_ATTR void onMmuChange(bool force = false) {
    uint32_t stsc = XTHAL_GET_CCOUNT();
    mmuChangeBmonMaxStart = max((bmonHead - bmonTail) & bmonArraySzMask, mmuChangeBmonMaxStart); 
    uint8_t newport = d000Write[_0x1ff];
    uint8_t portb = d000Write[_0x301]; 

    static bool lastBasicEn = true;
    static bool lastPbiEn = false;
    static bool lastPostEn = false;
    static bool lastOsEn = true;
    static bool lastXeBankEn = false;
    static int lastXeBankNr = 0;
    static int lastBankA0 = -1, lastBank80 = -1;

    // Figured this out - the native ram under the bank window can't be used usable during
    // bank switching becuase it catches the writes to extended ram and gets corrupted. 
    // Once a sparse base memory map is implemented, we will need to leave this 16K
    // mapped to emulated RAM.  
    bool xeBankEn = (portb & portbMask.xeBankEn) == 0;
    int xeBankNr = ((portb & 0x60) >> 3) | ((portb & 0x0c) >> 2); 
    if (lastXeBankEn != xeBankEn ||  lastXeBankNr != xeBankNr || force) { 
        uint8_t *mem;
        if (xeBankEn && (mem = extMem.getBank(xeBankNr)) != NULL) { 
            mmuMapRangeRWIsolated(_0x4000, _0x7fff, mem);
        } else { 
            mmuRemapBaseRam(_0x4000, _0x7fff);
        }
        lastXeBankEn = xeBankEn;
        lastXeBankNr = xeBankNr;
    }

    bool osEn = (portb & portbMask.osEn) != 0;
    bool pbiEn = (newport & pbiDeviceNumMask) != 0;
    if (lastOsEn != osEn || force) { 
        if (osEn) {
            mmuUnmapRange(_0xe000, _0xffff);
            mmuUnmapRange(_0xc000, _0xcfff);
        } else { 
            mmuRemapBaseRam(_0xe000, _0xffff);
            mmuRemapBaseRam(_0xc000, _0xcfff);
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
            mmuUnmapRange(_0x5000, _0x57ff);
        } else {
            mmuRemapBaseRam(_0x5000, _0x57ff);
        }
        lastPostEn = postEn;
    }

    bool basicEn = (portb & portbMask.basicEn) == 0;
    if (lastBasicEn != basicEn || lastBankA0 != atariCart.bankA0 || force) { 
        if (basicEn) { 
            mmuUnmapRange(_0xa000, _0xbfff);
        } else if (atariCart.bankA0 >= 0) {
            mmuMapRangeRO(_0xa000, _0xbfff, atariCart.image[atariCart.bankA0]);
        } else { 
            mmuRemapBaseRam(_0xa000, _0xbfff);
        }
        lastBasicEn = basicEn;
        lastBankA0 = atariCart.bankA0;
    }
    if (lastBank80 != atariCart.bank80 || force) { 
        if (atariCart.bank80 >= 0) {
            mmuMapRangeRO(_0x8000, _0x9fff, atariCart.image[atariCart.bank80]);
        } else { 
            mmuRemapBaseRam(_0x8000, _0x9fff);
        }
        lastBank80 = atariCart.bank80;
    }
}

IFLASH_ATTR void memoryMapInit() { 
    bzero(pageEnable, sizeof(pageEnable));
    mmuUnmapRange(0x0000, 0xffff);

    mmuAddBaseRam(0x0000, baseMemSz - 1, atariRam);
    mmuRemapBaseRam(0x0000, baseMemSz - 1);
    mmuAddBaseRam(0x4000, 0x7fff, extMemWindow);
    mmuRemapBaseRam(0x4000, 0x7fff);
    mmuAddBaseRam(0xd800, 0xdfff, d000BaseMem);
    mmuRemapBaseRam(0xd800, 0xdfff);

    mmuUnmapRange(0xd000, 0xd7ff);

    // map register writes for d000-d7ff to shadow write pages
    for(int b = pageNr(0xd000); b <= pageNr(0xd7ff); b++) { 
        pages[b | PAGESEL_CPU | PAGESEL_WR ] = &d000Write[0] + (b - pageNr(0xd000)) * pageSize; 
    }
    
#if pageSize <= 0x100    
    // enable reads from 0xd500-0xd5ff for emulating RTC-8 and other cartsel features 
    for(int b = pageNr(0xd500); b <= pageNr(0xd5ff); b++) { 
        pages[b | PAGESEL_CPU | PAGESEL_RD ] = &d000Read[0] + (b - pageNr(0xd000)) * pageSize; 
        pageEnable[b | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
    }

    // Map register reads for the page containing 0xd1ff so we can handle reads to newport/0xd1ff for implementing
    // PBI interrupt scheme 
    // pages[pageNr(0xd1ff) | PAGESEL_CPU | PAGESEL_RD ] = &D000Read[(pageNr(0xd1ff) - pageNr(0xd000)) * pageSize]; 
    // pageEnable[pageNr(0xd1ff) | PAGESEL_CPU | PAGESEL_RD] = pins.data.mask | pins.extSel.mask;
    // pageEnable[pageNr(0xd500) | PAGESEL_CPU | PAGESEL_RD ] |= pins.halt.mask;
#endif

    // enable the halt(ready) line in response to writes to 0xd301, 0xd1ff or 0xd500
    pageEnable[pageNr(0xd1ff) | PAGESEL_CPU | PAGESEL_WR ] |= bus.halt_.mask;
    pageEnable[pageNr(0xd301) | PAGESEL_CPU | PAGESEL_WR ] |= bus.halt_.mask;
    pageEnable[pageNr(0xd500) | PAGESEL_CPU | PAGESEL_WR ] |= bus.halt_.mask;

    // Intialize register shadow write memory to the default hardware reset values
    d000Write[0x301] = 0xff;
    d000Write[0x1ff] = 0x00;

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

static const DRAM_ATTR uint32_t interruptMaskNOT = ~bus.irq_.mask;
static const DRAM_ATTR uint32_t pbiDeviceNumMaskNOT = ~pbiDeviceNumMask;

IRAM_ATTR void raiseInterrupt() {
    if ((d000Write[_0x1ff] & pbiDeviceNumMask) != pbiDeviceNumMask
        && (d000Write[_0x301] & 0x1) != 0
    ) {
        deferredInterrupt = 0;  
        d000Read[_0x1ff] = pbiDeviceNumMask;
        atariRam[PDIMSK] |= pbiDeviceNumMask;
        pinReleaseMask &= interruptMaskNOT;
        pinDriveMask |= bus.irq_.mask;
        interruptRequested = 1;
    } else { 
        deferredInterrupt = 1;
    }
}

IRAM_ATTR void clearInterrupt() { 
    pinDriveMask &= interruptMaskNOT;
    pinReleaseMask |= bus.irq_.mask;
    interruptRequested = 0;
    busyWait6502Ticks(10);
    d000Read[_0x1ff] = 0x0;
    atariRam[PDIMSK] &= pbiDeviceNumMaskNOT;
}


IRAM_ATTR void enableBus() {
    busWriteDisable = 0;
    pinEnableMask = _0xffffffff; 
}

IRAM_ATTR void disableBus() { 
    busWriteDisable = 1;
    pinEnableMask = bus.halt_.mask;
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
DRAM_ATTR int wdTimeout = 60, ioTimeout = 30;
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

struct __attribute__((packed)) PbiIocb {
    uint8_t req;
    uint8_t cmd;
    uint8_t a;
    uint8_t x;

    uint8_t y;
    uint8_t carry;
    uint8_t result;
    uint8_t psp;

    uint16_t copybuf;
    uint16_t copylen;

    uint8_t kbcode;
    uint8_t sdmctl;
    uint8_t stackprog;
    uint8_t consol;
};

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
    int PBI_COPYBUF = 0xdc00;
} AtariDef;

DRAM_ATTR struct { 
    AtariDCB *dcb = (AtariDCB *)&atariRam[0x300];
    AtariIOCB *ziocb = (AtariIOCB *)&atariRam[0x20];
    AtariIOCB *iocb0 = (AtariIOCB *)&atariRam[0x320];
} atariMem;

DRAM_ATTR Hist2 profilers[numProfilers];
DRAM_ATTR int ramReads = 0, ramWrites = 0;

DRAM_ATTR const char *defaultProgram = 
#ifdef BOOT_SDX
        "10 PRINT \"HELLO FROM BASIC\" \233"
        "20 PRINT \"HELLO 2\" \233"
        "30 CLOSE #4:OPEN #4,8,0,\"J1:DUMPSCREEN\":PUT #4,0:CLOSE #4\233"
        "40 DOS\233 "
        "RUN \233"
#else
        //"1 DIM D$(255) \233"
        //"10 REM A=USR(1546, 1) \233"
        //"15 OPEN #1,4,0,\"J2:\" \233"
        //"20 GET #1,A  \233"
        //"38 CLOSE #1  \233"
        //"39 PRINT \"OK\" \233"
        //"40 GOTO 10 \233"
        //"41 OPEN #1,8,0,\"J\" \233"
        //"42 PUT #1,A + 1 \233"
        //"43 CLOSE #1 \233"
        //"50 PRINT \" -> \"; \233"
        //"52 PRINT COUNT; \233"
        //"53 COUNT = COUNT + 1 \233"
        //"60 OPEN #1,8,0,\"D1:DAT\":FOR I=0 TO 20:XIO 11,#1,8,0,D$:NEXT I:CLOSE #1 \233"
        //"61 TRAP 61: CLOSE #1: OPEN #1,4,0,\"D1:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        //"61 CLOSE #1: OPEN #1,4,0,\"D1:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        //"63 OPEN #1,4,0,\"D2:DAT\":FOR I=0 TO 10:XIO 7,#1,4,0,D$:NEXT I:CLOSE #1 \233"
        "60 TRAP 80:XIO 80,#1,0,0,\"D1:DIR D3:*.*/A\" \233"
        "70 TRAP 80:XIO 80,#1,0,0,\"D1:X.CMD\" \233"
        //"80 GOTO 10 \233"
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
DRAM_ATTR static int lastScreenShot = 0;
DRAM_ATTR int secondsWithoutWD = 0, lastIoSec = 0;
void IFLASH_ATTR dumpScreenToSerial(char tag, uint8_t *mem = NULL);
IRAM_ATTR uint8_t * checkRangeMapped(uint16_t start, uint16_t len);


uint8_t *mappedElseCopyIn(PbiIocb *pbiRequest, uint16_t addr, uint16_t len) { 
    if (checkRangeMapped(addr, len))
        return pages[pageNr(addr) + PAGESEL_CPU + PAGESEL_RD] + (addr & pageOffsetMask);
    
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

// CORE0 loop options 
struct AtariIO {
    uint8_t buf[256];
    int ptr = 0;
    int len = 0;
    string filename;
    AtariIO() { 
        strcpy((char *)buf, defaultProgram); 
        len = strlen((char *)buf);
    }
    inline IRAM_ATTR void open(const char *fn) { 
        ptr = 0; 
        filename = fn;
        watchDogCount++;
    }
    inline IRAM_ATTR void close(PbiIocb *p = NULL) {}
    inline IRAM_ATTR int get(PbiIocb *p = NULL) { 
        if (ptr >= len) return -1;
        return buf[ptr++];
    }
    inline IRAM_ATTR int put(uint8_t c, PbiIocb *pbiRequest) { 
        if (filename == DRAM_STR("J1:DUMPSCREEN")) { 
            uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
            uint16_t addr = savmsc;
            uint8_t *paddr = mappedElseCopyIn(pbiRequest, addr, 24 * 40 /*len = bytes of screen mem*/);
            if (paddr == NULL) 
                return 1;
            dumpScreenToSerial('S', paddr);
            lastScreenShot = elapsedSec;
        }
        return 1;
    }
};
//DRAM_ATTR 
AtariIO *fakeFile; 

#define STRUCT_LOG
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

struct StructLogs { 
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
} *structLogs;

DiskImage *atariDisks[8] = {NULL};

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
        UNIQUE_LOCAL(lastTsc) += UNIQUE_LOCAL(interval); \
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
    void saveScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        for(int i = 0; i < sizeof(screenMem); i++) { 
            screenMem[i] = atariRam[savmsc + i];
        }
    }
    Debounce consoleDebounce = Debounce(240 * 1000 * 30);
    Debounce keyboardDebounce = Debounce(240 * 1000 * 30);

    void clearScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        for(int i = 0; i < sizeof(screenMem); i++) { 
            atariRam[savmsc + i] = 0;
         }
    }
    void drawScreen() { 
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
    void restoreScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        for(int i = 0; i < sizeof(screenMem); i++) { 
            atariRam[savmsc + i] = screenMem[i];
        }
        atariRam[712] = 0;
        atariRam[710] = 148;
    }
    void writeAt(int x, int y, const string &s, bool inv) { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        if (x < 0) x = 20 - s.length() / 2;
        for(int i = 0; i < s.length(); i++) { 
            uint8_t c = s[i];
            if (c < 32) c += 64;
            else if (c < 96) c-= 32;
            atariRam[savmsc + y * 40 + x + i] = c + (inv ? 128 : 0);            
        }
    }
    void onConsoleKey(uint8_t key) {
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
    void pbi(PbiIocb *p) {
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
DRAM_ATTR static const uint32_t haltMaskNOT = ~bus.halt_.mask; 

void IRAM_ATTR halt6502() { 
    pinReleaseMask &= haltMaskNOT;
    pinDriveMask |= bus.halt_.mask;
    uint32_t stsc = XTHAL_GET_CCOUNT();
    for(int n = 0; n < 2; n++) { 
        int bHead = bmonHead;
        while(XTHAL_GET_CCOUNT() - stsc < bmonTimeout && bmonHead == bHead) {
            busyWait6502Ticks(1);
        }
    }
    pinDriveMask &= haltMaskNOT;
}

void IRAM_ATTR resume6502() {
    haltCount++; 
    pinDriveMask &= haltMaskNOT;
    pinReleaseMask |= bus.halt_.mask;
    uint32_t stsc = XTHAL_GET_CCOUNT();
    for(int n = 0; n < 2; n++) { 
        int bHead = bmonHead;
        while(XTHAL_GET_CCOUNT() - stsc < bmonTimeout && bmonHead == bHead) {
            busyWait6502Ticks(1);
        }
    }
    pinReleaseMask &= haltMaskNOT;
}

IFLASH_ATTR void screenMemToAscii(char *buf, int buflen, char c) { 
    bool inv = false;
    if (c & 0x80) {
        c -= 0x80;
        inv = true;
    };
    if (c < 64) c += 32;
    else if (c < 96) c -= 64;
    if (inv) 
        snprintf(buf, buflen, DRAM_STR("\033[7m%c\033[0m"), c);
    else 
        snprintf(buf, buflen, DRAM_STR("%c"), c);

}
void IFLASH_ATTR dumpScreenToSerial(char tag, uint8_t *mem/*= NULL*/) {
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    if (mem == NULL) {
        mem = checkRangeMapped(savmsc, 24 * 40);
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

extern int httpRequests;
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

bool IRAM_ATTR pbiCopyAndMapPages(PbiIocb *p, int startPage, int pages, uint8_t *mem) {
    if (!pbiReqCopyIn(p, startPage * pageSize, pages * pageSize, mem))
        return false;
    mmuMapRangeRW(startPage * pageSize, (startPage + pages) * pageSize - 1, mem);
    return true;
}

// called from a pbi command context to copy the data currently in native 6502 pages 
// into esp32 memory and map them after the system has booted 

bool IRAM_ATTR pbiCopyAndMapPagesIntoBasemem(PbiIocb *p, int startPage, int pages, uint8_t *mem) {
    if (!pbiCopyAndMapPages(p, startPage, pages, mem))
        return false;
    mmuAddBaseRam(startPage * pageSize, (startPage + pages) * pageSize - 1, mem);
    return true;           
}

// verify the a8 address range is mapped to internal esp32 ram and is continuous 
uint8_t *IRAM_ATTR checkRangeMapped(uint16_t addr, uint16_t len) { 
    for(int b = pageNr(addr); b <= pageNr(addr + len - 1); b++) { 
        if (pageEnable[PAGESEL_CPU + PAGESEL_RD + b] == 0) 
            return NULL;
        if (pages[PAGESEL_CPU + PAGESEL_WR + b] == &dummyRam[0]) 
            return NULL;
        // check mapping is continuous 
        uint8_t *firstPageMem = pages[PAGESEL_CPU + PAGESEL_WR + pageNr(addr)];
        int offset = (b - pageNr(addr)) * pageSize;
        if (pages[PAGESEL_CPU + PAGESEL_WR + b] != firstPageMem + offset)
            return NULL;
    }
    return pages[pageNr(addr) + PAGESEL_CPU + PAGESEL_RD] + (addr & pageOffsetMask);
}


#define TAG "smb"
#define CONFIG_SMB_USER "guest"
#define CONFIG_SMB_HOST "miner6.local"
#define CONFIG_SMB_PATH "pub"
#include "lwip/sys.h"
void startTelnetServer();
void telnetServerRun();

void smbReq() { 
    static uint8_t buf[1024];
    struct smb2_context *smb2;
    struct smb2_url *url;
    struct smb2fh *fh;
    int count;

    smb2 = smb2_init_context();
    if (smb2 == NULL) {
            ESP_LOGE(TAG, "Failed to init context");
            //while(1){ vTaskDelay(1); }
            return;
    }

    ESP_LOGI(TAG, "CONFIG_SMB_USER=[%s]",CONFIG_SMB_USER);
    ESP_LOGI(TAG, "CONFIG_SMB_HOST=[%s]",CONFIG_SMB_HOST);
    ESP_LOGI(TAG, "CONFIG_SMB_PATH=[%s]",CONFIG_SMB_PATH);  

    char smburl[64];
    sprintf(smburl, "smb://%s@%s/%s/esp-idf-cat.txt", CONFIG_SMB_USER, CONFIG_SMB_HOST, CONFIG_SMB_PATH);
    ESP_LOGI(TAG, "smburl=%s", smburl);

#if CONFIG_SMB_NEED_PASSWORD
        smb2_set_password(smb2, CONFIG_SMB_PASSWORD);
#endif

    url = smb2_parse_url(smb2, smburl);
    if (url == NULL) {
            ESP_LOGE(TAG, "Failed to parse url: %s", smb2_get_error(smb2));
            //while(1){ vTaskDelay(1); }
            smb2_destroy_context(smb2);
            return;
    }

    smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);

    if (smb2_connect_share(smb2, url->server, url->share, url->user) < 0) {
            ESP_LOGE(TAG, "smb2_connect_share failed. %s", smb2_get_error(smb2));
            smb2_destroy_url(url);
            smb2_destroy_context(smb2);
            //while(1){ vTaskDelay(1); }
    }

    ESP_LOGI(TAG, "url->path=%s", url->path);
    fh = smb2_open(smb2, url->path, O_RDONLY);
    if (fh == NULL) {
            ESP_LOGE(TAG, "smb2_open failed. %s", smb2_get_error(smb2));
            smb2_disconnect_share(smb2);
            smb2_destroy_url(url);
            smb2_destroy_context(smb2);
            //while(1){ vTaskDelay(1); }
    }

    int pos = 0;
    while ((count = smb2_pread(smb2, fh, buf, sizeof(buf), pos)) != 0) {
            if (count == -EAGAIN) {
                    continue;
            }
            if (count < 0) {
                    ESP_LOGE(TAG, "Failed to read file. %s", smb2_get_error(smb2));
                    break;
            }
            for(int n = 0; n < count; n++) putchar(buf[n]);
            //write(0, buf, count);
            pos += count;
    };
                            
    smb2_close(smb2, fh);
    smb2_disconnect_share(smb2);
    smb2_destroy_url(url);
    smb2_destroy_context(smb2);

}

IRAM_ATTR void wifiRun() { 
    static bool wifiInitialized = false;
    if (wifiInitialized == false) { 
        connectWifi(); // 82876 bytes 
        start_webserver();  //12516 bytes 
        //smbReq();
        startTelnetServer();
        for(int n = 0; n < sizeof(atariDisks)/sizeof(atariDisks[0]); n++) {
            // TMP disable until better error handling 
            if (atariDisks[n] != NULL) atariDisks[n]->start();
        }
        wifiInitialized = true;
    } else { 
        telnetServerRun();
    }
}

struct ScopedBlinkLED { 
    static uint8_t cur[3];// = {0};
    uint8_t prev[3];
    ScopedBlinkLED(uint8_t *set) {
        for(int n = 0; n < sizeof(cur); n++) prev[n] = cur[n];
        for(int n = 0; n < sizeof(cur); n++) cur[n] = set[n];
        NEWneopixelWrite(set[0], set[1], set[2]); 
    }
    ~ScopedBlinkLED() {  
        NEWneopixelWrite(prev[0], prev[1], prev[2]); 
        for(int n = 0; n < sizeof(cur); n++) cur[n] = prev[n];
    }
};
uint8_t ScopedBlinkLED::cur[3];
#define SCOPED_BLINK_LED(a,b,c) ScopedBlinkLED blink((uint8_t []){a,b,c});

int IRAM_ATTR handlePbiRequest2(PbiIocb *pbiRequest) {     
    //SCOPED_INTERRUPT_ENABLE(pbiRequest);
    structLogs->pbi.add(*pbiRequest);
    if (0) { 
        printf(DRAM_STR("IOCB: "));
        StructLog<PbiIocb>::printEntry(*pbiRequest);
        fflush(stdout);
    }
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

            uint8_t *paddr = checkRangeMapped(addr, dbyt);
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
        }
    } else if (pbiRequest->cmd == 8) { // IRQ
        clearInterrupt();
        SCOPED_BLINK_LED(0,0,20);

        // only do this once, don't try and re-map and follow screen mem around if it moves
        static bool screenMemMapped = false;
        if (!screenMemMapped) { 
            int savmsc = (atariRam[89] << 8) + atariRam[88];
            int len = 20 * 40;
            if (checkRangeMapped(savmsc, len) == NULL) {
                int numPages = pageNr(savmsc + len) - pageNr(savmsc) + 1;
                if(!pbiCopyAndMapPagesIntoBasemem(pbiRequest, pageNr(savmsc), numPages, screenMem))
                    return RES_FLAG_NEED_COPYIN;
                dumpScreenToSerial('M');
            }
            screenMemMapped = true;
        }
        wifiRun();
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
        SCOPED_BLINK_LED(0,20,0);
        sysMonitorRequested = 0;
        sysMonitor.pbi(pbiRequest);
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
    }
    return RES_FLAG_COMPLETE;
}

DRAM_ATTR int enableBusInTicks = 0;
PbiIocb *lastPbiReq;

void IRAM_ATTR handlePbiRequest(PbiIocb *pbiRequest) {  
    // Investigating halting the cpu instead of the stack-prog wait scheme
    // so far doens't work.
    
    // Assume pbi commands are always issued with the 6502 ready for a bus detach
    //pbiRequest->req = 0x2;

    //if (needSafeWait(pbiRequest))
    //    return;

//#define HALT_6502
#ifdef HALT_6502
    halt6502();
#endif
    {   
    SCOPED_INTERRUPT_ENABLE(pbiRequest);
    pbiRequest->result = 0;
    pbiRequest->result |= handlePbiRequest2(pbiRequest);
    {
        DRAM_ATTR static int lastPrint = -999;
        if (elapsedSec - lastPrint >= 2) {
            handleSerial();
            lastPrint = elapsedSec;
            static int lastIoCount = 0;
            printf(DRAM_STR("time %02d:%02d:%02d iocount: %8d (%3d) irqcount %d http %d "
                "halts %d evict %d/%d\n"), 
                elapsedSec/3600, (elapsedSec/60)%60, elapsedSec%60, ioCount,  
                ioCount - lastIoCount, 
                pbiInterruptCount, httpRequests, haltCount, extMem.evictCount, extMem.swapCount);
            fflush(stdout);
            lastIoCount = ioCount;
        }
        if (elapsedSec - lastScreenShot >= 90) {
            handleSerial();
            dumpScreenToSerial('Y');
            fflush(stdout);
            lastScreenShot = elapsedSec;
        }
    } 
    if (pbiRequest->consol == 0 || pbiRequest->kbcode == 0xe5 || sysMonitorRequested) 
        pbiRequest->result |= RES_FLAG_MONITOR;
    bmonTail = bmonHead;
    }
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
        do {
#ifdef FAKE_CLOCK
            break;
#endif
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
        pbiRequest->req = 0;
        atariRam[0x100 + pbiRequest->stackprog - 2] = 0;
    } else { 
        bmonTail = bmonHead;
        pbiRequest->req = 0;
    }
#ifdef HALT_6502
    busyWait6502Ticks(100);
    resume6502();
    busyWait6502Ticks(5);
#endif
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
        for(bTail = bmonTail; bTail != bmonHead; bTail = (bTail + 1) & bmonArraySzMask) { 
            uint32_t r0 = bmonArray[bTail] >> bmonR0Shift;
            if ((r0 & bus.rw.mask) == 0) {
                uint32_t lastWrite = (r0 & bus.addr.mask) >> bus.addr.shift;
                if (lastWrite == 0xd301 || lastWrite == 0xd1ff) 
                    mmuChange = true;
                if (lastWrite == 0xd830 || lastWrite == 0xd840) 
                    pbiReq = true;
            }
        }
        if (mmuChange) 
            onMmuChange();
        for(bTail = bmonTail; bTail != bmonHead; bTail = (bTail + 1) & bmonArraySzMask) { 
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
    PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];

    if (psram == NULL) {
        for(auto &t : bmonTriggers) t.count = 0;
    }

    uint32_t bmon = 0;
    bmonTail = bmonHead;
    while(1) {
        uint32_t stsc = XTHAL_GET_CCOUNT();
        const static DRAM_ATTR uint32_t bmonTimeout = 240 * 1000 * 50;
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

            PROFILE_BMON((bmonHead - bmonTail) & bmonArraySzMask);
            
            //bmonMax = max((bHead - bTail) & bmonArraySzMask, bmonMax);
            bmon = bmonArray[bTail] & bmonMask;
            bmonTail = (bTail + 1) & bmonArraySzMask;
        
            uint32_t r0 = bmon >> bmonR0Shift;

            uint16_t addr = (r0 & bus.addr.mask) >> bus.addr.shift;
            if ((r0 & bus.rw.mask) == 0) {
                uint32_t lastWrite = addr;
                if (lastWrite == _0xd301) 
                    onMmuChange();
                else if (lastWrite == _0xd1ff) 
                    onMmuChange();
                else if ((lastWrite & _0xff00) == _0xd500 && atariCart.accessD500(lastWrite)) 
                    onMmuChange();
                else if (lastWrite == _0xd830 && pbiRequest[0].req != 0) 
                    handlePbiRequest(&pbiRequest[0]);
                else if (lastWrite == _0xd840 && pbiRequest[1].req != 0) 
                    handlePbiRequest(&pbiRequest[1]);

                // these pages have pins.halt.mask set in pageEnable[] and will halt the 6502 on any write.
                // restart the 6502 now that onMmuChange has had a chance to run. 
                if (pageNr(lastWrite) == pageNr_d500 
                    || pageNr(lastWrite) == pageNr_d301
                    || pageNr(lastWrite) == pageNr_d1ff
                ) {
                    PROFILE_MMU(((bmonHead - bmonTail) & bmonArraySzMask) / 10);
                    bmonTail = bmonHead;
                    resume6502();
                }

            } else if ((r0 & bus.refresh_.mask) != 0) {
                uint32_t lastRead = addr;
                //if ((lastRead & _0xff00) == 0xd500 && atariCart.accessD500(lastRead)) 
                //    onMmuChange();
                //if (bankNr(lastWrite) == pageNr_d500)) resume6502(); 
                //if (lastRead == 0xFFFA) lastVblankTsc = XTHAL_GET_CCOUNT();
            }    

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

        if(0) {
            // We're missing some halts in the bmon queue, which makes sense. 
            // TODO: a more effecient way of detecting a halted 6502, or somehow 
            // ensure we don't miss ANY bmon traffic. 
            uint32_t stsc = XTHAL_GET_CCOUNT();
            pinReleaseMask |= bus.halt_.mask;
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
            pinReleaseMask &= (~bus.halt_.mask);
        }

#if 0 
        static uint8_t lastNewport = 0;
        static const DRAM_ATTR uint16_t _0x1ff = 0x1ff;
        static const DRAM_ATTR uint16_t _0x301 = 0x301;
        if (D000Write[_0x1ff] != lastNewport) { 
            lastNewport = D000Write[_0x1ff];
            onMmuChange();
        }
        static uint8_t lastPortb = 0;
        if (D000Write[0x301] != lastPortb) { 
            lastPortb = D000Write[0x301];
            onMmuChange();
        }
#endif
        if (deferredInterrupt 
            && (d000Write[_0x1ff] & pbiDeviceNumMask) != pbiDeviceNumMask
            && (d000Write[_0x301] & 0x1) != 0
        )
            raiseInterrupt();

        if (/*XXINT*/1 && (elapsedSec > 20 || ioCount > 1000)) {
            static uint32_t ltsc = 0;
            static const DRAM_ATTR int isrTicks = 240 * 1001 * 101; // 10Hz
            if (XTHAL_GET_CCOUNT() - ltsc > isrTicks) { 
                ltsc = XTHAL_GET_CCOUNT();
                raiseInterrupt();
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
                    pbiRequest->cmd = 8; // interrupt 
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

        static const DRAM_ATTR int keyTicks = 151 * 240 * 1000; // 150ms
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

            if (1 && elapsedSec == 15 && ioCount > 0) {
                //memcpy(&atariRam[0x0600], page6Prog, sizeof(page6Prog));
                //simulatedKeyInput.putKeys(DRAM_STR("CAR\233\233PAUSE 1\233\233\233E.\"J:X\"\233"));
                //simulatedKeyInput.putKeys("    \233DOS\233  \233DIR D2:\233");
#ifdef BOOT_SDX
                simulatedKeyInput.putKeys(DRAM_STR("-2:X\233"));
#else
                simulatedKeyInput.putKeys(DRAM_STR("E.\"J:X\"\233"));

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
                wdTimeout = ioTimeout = 300;
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

    printf("opt.fakeClock %d opt.histRunSec %d\n", opt.fakeClock, opt.histRunSec);
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
        uint16_t addr = r0 >> bus.addr.shift;
        char rw = (r0 & bus.rw.mask) != 0 ? 'R' : 'W';
        if ((r0 & bus.refresh_.mask) == 0) rw = 'F';
        uint8_t data = (bmonCopy[i] & 0xff);
        if (bmonExclude(bmonCopy[i])) continue;
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
    printf("atariRam[754] = %d\n", atariRam[754]);
    printf("pbiROM[0x100] = %d\n", pbiROM[0x100]);
    printf("reg[0xd301] = 0x%02x\n", d000Write[0x301]);
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
    printf("\n");

    dumpScreenToSerial('B');
#endif
    
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    int memReadErrors = (atariRam[0x609] << 24) + (atariRam[0x608] << 16) + (atariRam[0x607] << 16) + atariRam[0x606];
    printf("SUMMARY %-10.2f/%d e%d i%d d%d %s\n", millis()/1000.0, opt.histRunSec, memReadErrors, 
    pbiInterruptCount, ioCount, exitReason.c_str());
    printf("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    printf("GPIO_EN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_ENABLE_REG),REG_READ(GPIO_ENABLE1_REG)); 
    printf("extMem swaps %d evictions %d\n", extMem.swapCount, extMem.evictCount);
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
        for(auto p : gpios) pinMode(p, INPUT_PULLDOWN);
        pinDisable(bus.extDecode.pin);

        while(1) { 
            for(auto p : gpios) {
                printf("%02d:%d ", p, digitalRead(p));
            }
            printf("\n");
            delay(50);
        }
    }

    for(auto i : gpios) pinMode(i, INPUT);

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

    extMem.init(16, 3);
    //extMem.mapCompy192();
    extMem.mapRambo256();
#if 0
    for(int i = 0; i < 16; i++) {
        xeBankMem[i] = NULL;
    }
#ifdef RAMBO_XL256
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

    fakeFile = new AtariIO();
    structLogs = new StructLogs();
#ifdef BOOT_SDX
    atariDisks[0] = new DiskImageATR(spiffs_fs, "/toolkit.atr", true);
    atariCart.open("/SDX450_maxflash1.car");
#else
    atariDisks[0] = new DiskImageATR(spiffs_fs, "/d1.atr", true);
#endif
    atariDisks[1] = new DiskImageATR(spiffs_fs, "/d2.atr", true);
    atariDisks[2] = new DiskStitchImage<SmbConnection>("smb://miner6.local/pub");
    //atariDisks[2] = new DiskStitchImage<ProcFsConnection>();

    //atariCart.open("Joust.rom");
    //atariCart.open("Edass.car");
    //atariCart.open("SDX450_maxflash1.car");

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
    for(auto i : gpios) pinMode(i, INPUT);
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
        static const uint16_t testAddress = 0x2000;//0xd1ff;  
        for(int bit = 0; bit < 16; bit ++)
            pinMode(bus.addr.pin + bit, ((testAddress >> bit) & 1) == 1 ? INPUT_PULLUP : INPUT_PULLDOWN);

        //gpio_set_drive_capability((gpio_num_t)pins.clock.pin, GPIO_DRIVE_CAP_MAX);
        pinMode(bus.mpd.pin, INPUT_PULLDOWN);
        pinMode(bus.refresh_.pin, INPUT_PULLUP);
        //pinMode(pins.extDecode.pin, INPUT_PULLUP);
        pinMode(bus.extSel.pin, INPUT_PULLUP);
    }

    pinDisable(bus.extDecode.pin);
    for(int i = 0; i < 1; i++) { 
        printf("GPIO_IN_REG: %08" PRIx32 " %08" PRIx32 "\n", REG_READ(GPIO_IN_REG),REG_READ(GPIO_IN1_REG)); 
    }
    printf("freq %.4fMhz threshold %d halfcycle %d psram %p\n", 
        testFreq / 1000000.0, lateThresholdTicks, (int)halfCycleTicks, psram);

    gpio_matrix_in(bus.clock.pin, CORE1_GPIO_IN0_IDX, false);
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, 0);
    if(1) { 
        static dedic_gpio_bundle_handle_t bundleIn, bundleOut;
        int bundleB_gpios[] = {ledPin};
        dedic_gpio_bundle_config_t bundleB_config = {
            .gpio_array = bundleB_gpios,
            .array_size = sizeof(bundleB_gpios) / sizeof(bundleB_gpios[0]),
            .flags = {
                .out_en = 1
            },
        };
        ESP_ERROR_CHECK(dedic_gpio_new_bundle(&bundleB_config, &bundleOut));
        for(int i = 0; i < sizeof(bundleB_gpios) / sizeof(bundleB_gpios[0]); i++) { 
            //gpio_set_drive_capability((gpio_num_t)bundleB_gpios[i], GPIO_DRIVE_CAP_MAX);
        }
    }
    NEWneopixelWrite(0, 20, 0);
    //gpio_matrix_out(48, CORE1_GPIO_OUT0_IDX, false, false);
    digitalWrite(bus.irq_.pin, 1);
    pinMode(bus.irq_.pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(bus.irq_.pin, 1);
    //gpio_matrix_out(pins.interrupt.pin, CORE1_GPIO_OUT0_IDX, false, false);
    pinMode(bus.irq_.pin, OUTPUT_OPEN_DRAIN);
    pinMode(bus.halt_.pin, OUTPUT_OPEN_DRAIN);
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.irq_.mask);
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, bus.halt_.mask);
    digitalWrite(bus.irq_.pin, 0);
    digitalWrite(bus.halt_.pin, 0);
    for(int i = 0; i < 8; i++) { 
        pinMode(bus.data.pin + i, OUTPUT); // TODO: Investigate OUTPUT_OPEN_DRAIN doesn't work, would enable larger page sizes if it did 
    }
    clearInterrupt();
    memoryMapInit();
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
