#pragma once
#include <inttypes.h>
#include <stddef.h>

class DiskImage { 
    public:
    virtual size_t read(uint8_t *buf, size_t sector) = 0;
    virtual size_t write(const uint8_t *buf, size_t sector) = 0;
    virtual int sectorSize() = 0;
    virtual int sectorCount() = 0;
    virtual bool valid() = 0;
    //virtual void start() {}
};

// 16-byte ATR file header 
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

