#include "esp_heap_caps.h"
#include "cartridge.h"
#include "spiffs.h"
#include "pinDefs.h"
#include "mmu.h"
#include "util.h"

// TODO: rename to cartMmuBanks[] to distinguish MMU Banks from Cartridge Banks
//DRAM_ATTR BankL1Entry *cartBanks[256] = {0};

// TODO: add this to mmu.h, remove similarily named local variables in code 
#define pageInBank(p) ((p) & pageInBankMask)

// Caution: very confusing mixed use of 8K Cartridge Banks, and 16K MMU banks
// AtariCart::image[n] holds informaton about 8K cart banks,
// while AtariCart[n].mmuData holds data for one 16K MMU page data needed to map each Cartridge Bank

void IFLASH_ATTR AtariCart::initMmuBank() { 
    uint16_t cartStart = header.type == Std16K ? 0x8000 : 0xa000;
    //uint16_t cartSize = header.type == Std16K ? 0x4000 : 0x2000;
    // Set up MMU banks for cartridge bank switching entries in image[] for each Cartrige Bank in 0..this->bankCount
    if (header.type == AtMax128) { 
        for(int cBank = 0; cBank < bankCount; cBank++) { 
            image[cBank].mmuData = banksL1[page2bank(pageNr(0x8000))];
            for(int pageInCartBank = 0; pageInCartBank <= pageNr(CartBankSize - 1); pageInCartBank++) {
                int pageInMmuBank = pageInBank(pageNr(cartStart + pageInCartBank * pageSize));
                image[cBank].mmuData.pages[pageInMmuBank | PAGESEL_CPU | PAGESEL_RD] = image[cBank].mem + (pageSize * pageInCartBank);
                image[cBank].mmuData.pages[pageInMmuBank | PAGESEL_CPU | PAGESEL_WR] = dummyRam;
                image[cBank].mmuData.ctrl [pageInMmuBank | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
                image[cBank].mmuData.ctrl [pageInMmuBank | PAGESEL_CPU | PAGESEL_WR] = 0;
            } 
            mmuState.cartBanks[cBank] = &image[cBank].mmuData;
        }
        // Set the rest of cartBanks[] array to MMU banks that map default RAM, ie: cartridge switched off
        for(int cb = bankCount; cb < ARRAYSZ(mmuState.cartBanks); cb++) 
            mmuState.cartBanks[cb] = &banksL1[page2bank(pageNr(0x8000))];
    
    // For standard 8K or 16K cartridges, set up image[0] to map the entire cartridge, then reference it in the 
    // entire cartBanks[] array.  ie: cartridge is always mapped no matter what is written to d500 cartridge control
    } else if (header.type == Std8K || header.type == Std16K) { 
        image[0].mmuData = banksL1[page2bank(pageNr(0x8000))];
        for(int cBank = 0; cBank < bankCount; cBank++) { 
            for(int pageInCartBank = 0; pageInCartBank <= pageNr(CartBankSize - 1); pageInCartBank++) {
                int pageInMmuBank = pageInBank(pageNr(cartStart + cBank * CartBankSize + pageInCartBank * pageSize));
                image[0].mmuData.pages[pageInMmuBank | PAGESEL_CPU | PAGESEL_RD] = image[cBank].mem + (pageSize * pageInCartBank);
                image[0].mmuData.pages[pageInMmuBank | PAGESEL_CPU | PAGESEL_WR] = dummyRam;
                image[0].mmuData.ctrl [pageInMmuBank | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
                image[0].mmuData.ctrl [pageInMmuBank | PAGESEL_CPU | PAGESEL_WR] = 0;
            } 
        }
        for(int b = 0; b < ARRAYSZ(mmuState.cartBanks); b++) 
            mmuState.cartBanks[b] = &image[0].mmuData;

    // No cartridge, map cartBanks[] array to normal RAM
    } else { 
        for(int b = 0; b < ARRAYSZ(mmuState.cartBanks); b++) 
            mmuState.cartBanks[b] = &banksL1[page2bank(pageNr(0x8000))];
    }
    if (bankCount > 0) 
        mmuState.banks[bankL1Nr(0x8000)] = mmuState.cartBanks[0];
}

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
        image[i].mem = (uint8_t *)heap_caps_malloc(CartBankSize, MALLOC_CAP_INTERNAL);
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
        int r = SPIFFS_read(fs, fd, image[i].mem, CartBankSize);
        if (r != CartBankSize) { 
            printf("AtariCart::read('%s') failed (0x%x != 0x%x)\n", f, r, CartBankSize);
            while(--i > 0)
                heap_caps_free(image[i].mem);
            heap_caps_free(image);
            image = NULL;
            SPIFFS_close(fs, fd);
            return;
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
