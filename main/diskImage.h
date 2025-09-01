#pragma once
#include <inttypes.h>
#include <stddef.h>

#include "esp_spiffs.h"
#include "spiffs.h"

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

class DiskImage { 
    public:
    virtual size_t read(uint8_t *buf, size_t sector) = 0;
    virtual size_t write(uint8_t *buf, size_t sector) = 0;
    virtual int sectorSize() = 0;
    virtual int sectorCount() = 0;
    virtual bool valid() = 0;
};

class DiskImageATR : public DiskImage {
public:
    string filename;
    AtrImageHeader header;
    uint8_t *image;
    spiffs *fs;
    spiffs_file fd;

    DiskImageATR(spiffs *spiffs_fs, const char *f, bool cache) : fs(spiffs_fs) { open(f, cache); }
    void open(const char *f, bool cacheInPsram) {
        image = NULL;
        spiffs_stat stat;
        if (SPIFFS_stat(fs, f, &stat) < 0 ||
            (fd = SPIFFS_open(fs, f, SPIFFS_O_RDWR, 0)) < 0) { 
            printf("AtariDisk::open('%s'): file open failed\n", f);
            return;
        }
        size_t fsize = stat.size;
        int r = SPIFFS_read(fs, fd, &header, sizeof(header));
        if (r != sizeof(header) || header.magic != 0x0296) { 
            printf("DiskImage::open('%s'): bad file or bad atr header\n", f);
            SPIFFS_close(fs, fd);
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
                SPIFFS_close(fs, fd);
                return;
            }
            printf("Opened '%s' file size %zu bytes, reading data: ", f, fsize);
            SPIFFS_lseek(fs, fd, sizeof(header), SPIFFS_SEEK_SET);
            int r = SPIFFS_read(fs, fd, image, dataSize);
            SPIFFS_close(fs, fd); 
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
    virtual bool valid() { return header.magic == 0x0296; }
    virtual int sectorSize() { return header.sectorSize; }
    virtual int sectorCount() { return (header.pars + header.parsHigh * 256) * 0x10 / header.sectorSize; }
    void IRAM_ATTR close() {
        if (image != NULL) {
            heap_caps_free(image);
            image = NULL;
        } else { 
            SPIFFS_close(fs, fd); 
        }
    }
    virtual size_t read(uint8_t *buf, size_t sector) {
        int secSize = header.sectorSize;
        // first 3 sectors are always 128 bytes even on DD disks
        if (sector <= 3) secSize = 128;
        int offset = (sector - 1) * secSize;
        if (sector > 3) offset -= 3 * (secSize - 128);
        if(image != NULL) {
            for(int n = 0; n < secSize; n++) 
                buf[n] = image[offset + n];
            return secSize;
        } else if(filename.length() != 0) {
            SPIFFS_lseek(fs, fd, offset + sizeof(header), SPIFFS_SEEK_SET);
            return SPIFFS_read(fs, fd, buf, secSize);                                    
        }
        return 0;
    }
    virtual size_t write(uint8_t *buf, size_t sector) { 
        int secSize = header.sectorSize;
        // first 3 sectors are always 128 bytes even on DD disks
        if (sector <= 3) secSize = 128;
        int offset = (sector - 1) * secSize;
        if (sector > 3) offset -= 3 * (secSize - 128);
        if(image != NULL) {
            for(int n = 0; n < secSize; n++) image[offset + n] = buf[n];
            return secSize;
        } else if(filename.length() != 0) {
            //printf("write %d at %d:\n", len, offset); 
            SPIFFS_lseek(fs, fd, offset + sizeof(header), SPIFFS_SEEK_SET);
            size_t r = SPIFFS_write(fs, fd, buf, secSize);  
            //SPIFFS_flush(fs, fd);
            return r;
        }
        return 0;
    }
};

