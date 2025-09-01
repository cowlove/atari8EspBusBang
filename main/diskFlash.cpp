#include "diskFlash.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "spiffs.h"

void DiskImageATR::open(const char *f, bool cacheInPsram) {
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
bool IRAM_ATTR DiskImageATR::valid() { return header.magic == 0x0296; }

int IRAM_ATTR DiskImageATR::sectorSize() { return header.sectorSize; }

int IRAM_ATTR DiskImageATR::sectorCount() { return (header.pars + header.parsHigh * 256) * 0x10 / header.sectorSize; }

void DiskImageATR::close() {
    if (image != NULL) {
        heap_caps_free(image);
        image = NULL;
    } else { 
        SPIFFS_close(fs, fd); 
    }
}

size_t IRAM_ATTR DiskImageATR::read(uint8_t *buf, size_t sector) {
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

size_t IRAM_ATTR DiskImageATR::write(uint8_t *buf, size_t sector) { 
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
