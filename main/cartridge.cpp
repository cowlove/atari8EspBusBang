#include "esp_heap_caps.h"
#include "cartridge.h"
#include "spiffs.h"
#include "pinDefs.h"
#include "mmu.h"
#include "util.h"
#include "gzipImage.h"

#include <string.h>

namespace {
constexpr size_t MaxCartImageSize = 128 * 1024;
}

// TODO: add this to mmu.h, remove similarily named local variables in code 
#define pageInL1Block(p) ((p) & pageInL1BlockMask)

// Caution: cartridge banks are 8K; L1Block mappings are 16K.
// AtariCart::image[n] holds cartridge-bank data and its corresponding L1Block mapping.

void IFLASH_ATTR AtariCart::initMmuBlocks() {
    uint16_t cartStart = (header.type == Std16K || header.type == MegaCart128) ? 0x8000 : 0xa000;
    //uint16_t cartSize = header.type == Std16K ? 0x4000 : 0x2000;
    // Set up L1Block mappings for each cartridge-bank entry in image[].
    if (header.type == MegaCart128) {
        for (int cBank = 0; cBank < bankCount; cBank++) {
            image[cBank].l1Block = l1Blocks[page2L1Block(pageNr(0x8000))];
            for (int pageInCartBank = 0; pageInCartBank <= pageNr(MegaCartBankSize - 1); pageInCartBank++) {
                int pageInL1BlockIndex = pageInL1Block(pageNr(cartStart + pageInCartBank * pageSize));
                image[cBank].l1Block.pages[pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_RD] = image[cBank].mem + (pageSize * pageInCartBank);
                image[cBank].l1Block.pages[pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_WR] = dummyRam;
                image[cBank].l1Block.ctrl[pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
                image[cBank].l1Block.ctrl[pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_WR] = 0;
            }
            mmuState.cartBanks[cBank] = &image[cBank].l1Block;
        }
        for (int cb = bankCount; cb < ARRAYSZ(mmuState.cartBanks); cb++)
            mmuState.cartBanks[cb] = &l1Blocks[page2L1Block(pageNr(0x8000))];
    } else if (header.type == AtMax128) {
        for(int cBank = 0; cBank < bankCount; cBank++) { 
            image[cBank].l1Block = l1Blocks[page2L1Block(pageNr(0x8000))];
            for(int pageInCartBank = 0; pageInCartBank <= pageNr(CartBankSize - 1); pageInCartBank++) {
                int pageInL1BlockIndex = pageInL1Block(pageNr(cartStart + pageInCartBank * pageSize));
                image[cBank].l1Block.pages[pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_RD] = image[cBank].mem + (pageSize * pageInCartBank);
                image[cBank].l1Block.pages[pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_WR] = dummyRam;
                image[cBank].l1Block.ctrl [pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
                image[cBank].l1Block.ctrl [pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_WR] = 0;
            } 
            mmuState.cartBanks[cBank] = &image[cBank].l1Block;
        }
        // Set the rest of cartBanks[] array to L1Block mappings that map default RAM, ie: cartridge switched off
        for(int cb = bankCount; cb < ARRAYSZ(mmuState.cartBanks); cb++) 
            mmuState.cartBanks[cb] = &l1Blocks[page2L1Block(pageNr(0x8000))];
    
    // For standard 8K or 16K cartridges, set up image[0] to map the entire cartridge, then reference it in the 
    // entire cartBanks[] array.  ie: cartridge is always mapped no matter what is written to d500 cartridge control
    } else if (header.type == Std8K || header.type == Std16K) { 
        image[0].l1Block = l1Blocks[page2L1Block(pageNr(0x8000))];
        for(int cBank = 0; cBank < bankCount; cBank++) { 
            for(int pageInCartBank = 0; pageInCartBank <= pageNr(CartBankSize - 1); pageInCartBank++) {
                int pageInL1BlockIndex = pageInL1Block(pageNr(cartStart + cBank * CartBankSize + pageInCartBank * pageSize));
                image[0].l1Block.pages[pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_RD] = image[cBank].mem + (pageSize * pageInCartBank);
                image[0].l1Block.pages[pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_WR] = dummyRam;
                image[0].l1Block.ctrl [pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_RD] = bus.data.mask | bus.extSel.mask;
                image[0].l1Block.ctrl [pageInL1BlockIndex | PAGESEL_CPU | PAGESEL_WR] = 0;
            } 
        }
        for(int b = 0; b < ARRAYSZ(mmuState.cartBanks); b++) 
            mmuState.cartBanks[b] = &image[0].l1Block;

    // No cartridge, map cartBanks[] array to normal RAM
    } else { 
        for(int b = 0; b < ARRAYSZ(mmuState.cartBanks); b++) 
            mmuState.cartBanks[b] = &l1Blocks[page2L1Block(pageNr(0x8000))];
    }
    if (bankCount > 0) 
        mmuState.l1Blocks[l1BlockNr(0x8000)] = mmuState.cartBanks[0];
}

void IFLASH_ATTR AtariCart::open(spiffs *fs, const char *f) {
    if (imageData) heap_caps_free(imageData);
    if (image) heap_caps_free(image);
    imageData = NULL;
    image = NULL;

    bank80 = bankA0 = -1;
    bankCount = 0;
    size = 0;
    spiffs_stat stat;
    spiffs_file fd;

    if (SPIFFS_stat(fs, f, &stat) < 0 ||
        (fd = SPIFFS_open(fs, f, SPIFFS_O_RDONLY, 0)) < 0) { 
        printf("AtariCart::open('%s'): file open failed\n", f);
        return;
    }
    size_t fileSize = stat.size;
    size_t payloadOffset = 0;
    size_t decodedSize = fileSize;
    GzipImageInfo gzipInfo;
    bool compressed = hasGzipSuffix(f);

    if (compressed) {
        if (!gzipImageInflate(fs, fd, fileSize, nullptr, 0,
                              MaxCartImageSize + sizeof(header), gzipInfo)) {
            SPIFFS_close(fs, fd);
            printf("AtariCart::open('%s'): invalid or unsupported gzip image\n", f);
            return;
        }
        decodedSize = gzipInfo.outputSize;
        if (decodedSize >= sizeof(header) && (decodedSize & 0x1fff) == sizeof(header)) {
            memcpy(&header, gzipInfo.prefix, sizeof(header));
            payloadOffset = sizeof(header);
        } else if (decodedSize == 0x2000) {
            header.type = Std8K;
        } else if (decodedSize == 0x4000) {
            header.type = Std16K;
        } else {
            SPIFFS_close(fs, fd);
            printf("AtariCart::open('%s'): decompressed ROM size isn't supported\n", f);
            return;
        }
    } else if ((fileSize & 0x1fff) == sizeof(header)) {
        int r = SPIFFS_read(fs, fd, &header, sizeof(header));
        if (r != sizeof(header) ||
            (header.type != MegaCart128
                && header.type != AtMax128
                && header.type != Std8K
                && header.type != Std16K) 
            /*|| header.magic != CAR_FILE_MAGIC */) { 
            SPIFFS_close(fs, fd);
            printf("AtariCart::open('%s'): bad file, header, or type\n", f);
            return;
        }
        payloadOffset = sizeof(header);
    } else { 
        if (fileSize == 0x2000) header.type = Std8K;
        else if (fileSize == 0x4000) header.type = Std16K;
        else {
            SPIFFS_close(fs, fd);
            printf("AtariCart::open('%s'): raw ROM file isn't 8K or 16K in size\n", f);
            return;
        }
    }
    size = decodedSize - payloadOffset;
    if (size == 0 || size > MaxCartImageSize || (size % CartBankSize) != 0 ||
        (header.type != MegaCart128 && header.type != AtMax128 && header.type != Std8K && header.type != Std16K) ||
        (header.type == MegaCart128 && size != MaxCartImageSize)) {
        SPIFFS_close(fs, fd);
        size = 0;
        printf("AtariCart::open('%s'): unsupported cartridge size or type\n", f);
        return;
    }
        printf("AtariCart::open('%s'): ROM size %u%s\n", f, static_cast<unsigned>(size),
           compressed ? " (gzip)" : "");

    bankCount = size / (header.type == MegaCart128 ? MegaCartBankSize : CartBankSize);
    image = static_cast<CartBankInfo *>(heap_caps_calloc(bankCount, sizeof(CartBankInfo), MALLOC_CAP_INTERNAL));
    imageData = static_cast<uint8_t *>(heap_caps_malloc(decodedSize + (compressed ? 1 : 0), MALLOC_CAP_INTERNAL));
    if (!image || !imageData) {
        printf("AtariCart::open('%s'): dram heap_caps_malloc() failed!\n", f);
        if (image) heap_caps_free(image);
        if (imageData) heap_caps_free(imageData);
        image = nullptr;
        imageData = nullptr;
        bankCount = 0;
        size = 0;
        SPIFFS_close(fs, fd);
        return;
    }

    bool loaded = false;
    if (compressed) {
        if (SPIFFS_lseek(fs, fd, 0, SPIFFS_SEEK_SET) >= 0) {
            GzipImageInfo loadedInfo;
            loaded = gzipImageInflate(fs, fd, fileSize, imageData, decodedSize + 1,
                                      MaxCartImageSize + sizeof(header), loadedInfo) &&
                     loadedInfo.outputSize == gzipInfo.outputSize &&
                     loadedInfo.outputCrc == gzipInfo.outputCrc &&
                     memcmp(loadedInfo.prefix, gzipInfo.prefix, sizeof(gzipInfo.prefix)) == 0;
        }
    } else {
        loaded = SPIFFS_lseek(fs, fd, payloadOffset, SPIFFS_SEEK_SET) >= 0 &&
                 SPIFFS_read(fs, fd, imageData + payloadOffset, size) == static_cast<int>(size);
    }
    if (!loaded) {
        printf("AtariCart::open('%s'): image read/decompression failed\n", f);
        heap_caps_free(imageData);
        heap_caps_free(image);
        imageData = nullptr;
        image = nullptr;
        bankCount = 0;
        size = 0;
        SPIFFS_close(fs, fd);
        return;
    }
    for (int i = 0; i < bankCount; i++) {
        const size_t cartBankSize = header.type == MegaCart128 ? MegaCartBankSize : CartBankSize;
        image[i].mem = imageData + payloadOffset + i * cartBankSize;
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
