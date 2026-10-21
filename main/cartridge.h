#pragma once
//#pragma GCC optimize("O1")

#include <inttypes.h>
#include <string>
#include "esp_attr.h"

using std::string;

#include "mmu.h"


#ifndef IFLASH_ATTR 
#define IFLASH_ATTR 
#endif

#define CAR_FILE_MAGIC  ((int)'C' + ((int)'A' << 8) + ((int)'R' << 16) + ((int)'T' << 24))
struct __attribute__((packed)) CARFileHeader {
    uint32_t magic = 0;
    uint8_t unused[3];
    uint8_t type = 0; 
    uint32_t cksum;
    uint32_t unused2;
};

struct spiffs_t; 

struct AtariCart {
    enum CarType { 
        None = 0,
        MegaCart128 = 29,
        AtMax128 = 41,
        Std8K = 1,
        Std16K = 2,
    };
    struct CartBankInfo {
        L1Block l1Block;
        uint8_t *mem;
    };
    static constexpr int CartBankSize = 0x2000;
    static constexpr int MegaCartBankSize = 0x4000;
    string filename;
    CARFileHeader header;
    CartBankInfo *image = NULL;
    size_t size = 0;
    int bankCount = 0;
    int type = -1;
    int bank80 = -1, bankA0 = -1;
    void close();
    void open(spiffs_t *fs, const char *f);
    bool IRAM_ATTR inline accessD500(uint16_t addr) {
        if (header.type == MegaCart128)
            return image != NULL && addr == 0xd500;
        int8_t b = (addr & 0xff); 
        if (image != NULL && b != bankA0 /*&& (b & 0xe0) == 0*/) { 
            bankA0 = b < bankCount ? b : -1;
            return true;
        }
        return false;
    }
    void initMmuBlocks();
};

extern DRAM_ATTR AtariCart atariCart;
