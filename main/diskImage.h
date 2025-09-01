#pragma once
#include <inttypes.h>
#include <stddef.h>

class DiskImage { 
    public:
    virtual size_t read(uint8_t *buf, size_t sector) = 0;
    virtual size_t write(uint8_t *buf, size_t sector) = 0;
    virtual int sectorSize() = 0;
    virtual int sectorCount() = 0;
    virtual bool valid() = 0;
};


