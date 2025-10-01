#pragma once
#include <inttypes.h>
#include <string>
#include "esp_attr.h"

using std::string;

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
    void IFLASH_ATTR open(spiffs_t *fs, const char *f);
    bool IRAM_ATTR inline accessD500(uint16_t addr) {
        int b = (addr & 0xff); 
        if (image != NULL && b != bankA0 && (b & 0xe0) == 0) { 
            bankA0 = b < bankCount ? b : -1;
            return true;
        }
        return false;
    }
};

extern DRAM_ATTR AtariCart atariCart;
