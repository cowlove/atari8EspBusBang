#pragma once

#include <string>
using std::string;

#include "esp_attr.h"
#include "pbi.h"
#include "main.h"


extern DRAM_ATTR const char *defaultProgram;

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
extern DRAM_ATTR AtariIO *fakeFile; 
