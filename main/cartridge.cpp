#include "esp_heap_caps.h"
#include "cartridge.h"
#include "spiffs.h"
#include "pinDefs.h"

void IFLASH_ATTR AtariCart::open(spiffs *fs, const char *f) {
    if (image != NULL) { 
        for(int i = 0; i < bankCount; i++) {
            if (image[i].mem != NULL) 
                heap_caps_free(image[i].mem);
        }
        heap_caps_free(image);
        image = NULL;
    }

    bank80 = bankA0 = -1;
    bankCount = 0;
    spiffs_stat stat;
    spiffs_file fd;

    if (SPIFFS_stat(fs, f, &stat) < 0 ||
        (fd = SPIFFS_open(fs, f, SPIFFS_O_RDONLY, 0)) < 0) { 
        printf("AtariCart::open('%s'): file open failed\n", f);
        return;
    }
    size_t fsize = stat.size;
    if ((fsize & 0x1fff) == sizeof(header)) {
        int r = SPIFFS_read(fs, fd, &header, sizeof(header));
        if (r != sizeof(header) || 
            (header.type != AtMax128 
                && header.type != Std8K
                && header.type != Std16K) 
            /*|| header.magic != CAR_FILE_MAGIC */) { 
            SPIFFS_close(fs, fd);
            printf("AtariCart::open('%s'): bad file, header, or type\n", f);
            return;
        }
        size = fsize - sizeof(header);
    } else { 
        size = fsize;
        if (size == 0x2000) header.type = Std8K;
        else if (size == 0x4000) header.type = Std16K;
        else {
            SPIFFS_close(fs, fd);
            printf("AtariCart::open('%s'): raw ROM file isn't 8K or 16K in size\n", f);
            return;
        }
    }
    printf("AtariCart::open('%s'): ROM size %d\n", f, size); 

    // TODO: malloc 8k banks instead of one large chunk
    bankCount = size >> 13;
    image = (BankInfo *)heap_caps_malloc(bankCount * sizeof(BankInfo), MALLOC_CAP_INTERNAL);
    if (image == NULL) {
        printf("AtariCart::open('%s'): dram heap_caps_malloc() failed!\n", f);
        return;
    }            
    for (int i = 0; i < bankCount; i++) {
        image[i].mem = (uint8_t *)heap_caps_malloc(0x2000, MALLOC_CAP_INTERNAL);
        if (image[i].mem == NULL) {
            printf("AtariCart::open('%s'): dram heap_caps_malloc() failed bank %d/%d!\n", f, i, bankCount);
            heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
            while(--i > 0)
                heap_caps_free(image[i].mem);
            heap_caps_free(image);
            image = NULL;
            SPIFFS_close(fs, fd);
            return;
        }
        int r = SPIFFS_read(fs, fd, image[i].mem, 0x2000);
        if (r != 0x2000) { 
            printf("AtariCart::read('%s') failed (%d != 0x2000)\n", f, r);
            while(--i > 0)
                heap_caps_free(image[i].mem);
            heap_caps_free(image);
            image = NULL;
            SPIFFS_close(fs, fd);
            return;
        }
        for(int p = 0; p < pagesPerBank; p++) { 
            image[i].mmuData.pages[p] = image[i].mem + (pageSize * p);
            image[i].mmuData.ctrl[p] = bus.data.mask | bus.extSel.mask;
        }
    }
    SPIFFS_close(fs, fd);
    if (header.type == Std16K) {
        bank80 = 0;
        bankA0 = 1;
    } else { 
        bankA0 = 0;
        bank80 = -1;
    }
}   


AtariCart atariCart;
