#pragma once
#include <inttypes.h>
#include <stddef.h>
#include <string>

#include "spiffs.h"
#include "diskImage.h"

using std::string;

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

class DiskImageATR : public DiskImage {
public:
    string filename;
    AtrImageHeader header;
    uint8_t *image;
    spiffs *fs;
    spiffs_file fd;

    DiskImageATR(spiffs *spiffs_fs, const char *f, bool cache) : fs(spiffs_fs) { open(f, cache); }
    void close();
    void open(const char *f, bool cacheInPsram);
    bool valid();
    int sectorSize();    virtual int sectorCount();
    size_t read(uint8_t *buf, size_t sector);
    size_t write(uint8_t *buf, size_t sector);
};
