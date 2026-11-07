// Which 32 KB internal SRAM bank each core's hot data sits in, checked at
// boot: the data-side counterpart of tools/verify_iram_hotspots.py (which
// keeps the two cores' hot *code* in separate banks).
//
// Two masters touching one bank in the same cycle stall one of them, so the
// core 1 bus loop's data should share no bank with data core 0 hammers.
// Three classes:
//   core1   only the bus loop: the MMU tables and the pages they map, the
//           D000 page shadows, lastPageOffset, core 1's stack
//   shared  both cores, hard: bmonArray
//   core0   core 0's busiest data: the i2slink block (buffers, descriptors,
//           and the DMA writing them)
//   info    printed, never checked: core 0's interrupt stack (core 0 has
//           interrupts masked whenever core 1 is running bus traffic)
// Rules: core0 never shares a bank with core1, and the i2slink block never
// shares one with bmonArray.

#include "hotData.h"

#ifndef CSIM

#include <stdint.h>
#include <stdio.h>

#include "sdkconfig.h"

#include "bmon.h"
#include "mmu.h"
#if CONFIG_A8BB_SIDECAR_ENABLE
#include "sidecarHost.h"
#endif

extern uint8_t app_cpu_stack[4096];
extern "C" uint8_t port_IntStack[];

namespace {

constexpr uintptr_t kBankBase = 0x3FC80000;   // bank 0, as IRAM 0x40370000
constexpr uintptr_t kSramStart = 0x3FC88000;  // first DRAM-addressable byte
constexpr uintptr_t kSramEnd = 0x3FD00000;
constexpr uintptr_t kBankSize = 0x8000;

enum Class { kCore1, kShared, kCore0, kInfo, kClassCount };
const char *const kClassName[kClassCount] = {"core1", "shared", "core0", "info"};

// Banks [start, start + size) touches, as a bit mask; bit 31 for any byte
// outside internal SRAM (PSRAM, flash, ROM).
uint32_t banksOf(uintptr_t start, size_t size) {
    if (!start || !size) {
        return 0;
    }
    uint32_t mask = 0;
    const uintptr_t end = start + size;
    for (uintptr_t at = start; at < end; at = (at / kBankSize + 1) * kBankSize) {
        if (at < kSramStart || at >= kSramEnd) {
            mask |= 1u << 31;
        } else {
            mask |= 1u << ((at - kBankBase) / kBankSize);
        }
    }
    return mask;
}

void printBanks(uint32_t mask) {
    bool any = false;
    for (int bank = 0; bank < 31; ++bank) {
        if (mask & (1u << bank)) {
            printf("%s%d", any ? "," : "", bank);
            any = true;
        }
    }
    if (mask & (1u << 31)) {
        printf("%sext", any ? "," : "");
    }
}

struct Range {
    Class cls;
    const char *name;
    uint32_t banks;
};

constexpr int kMaxRanges = 16;
Range ranges[kMaxRanges];
int rangeCount = 0;

void add(Class cls, const char *name, uintptr_t start, size_t size) {
    const uint32_t banks = banksOf(start, size);
    printf("  %-6s %-22s %08x..%08x  bank ", kClassName[cls], name, (unsigned)start,
           (unsigned)(start + size));
    printBanks(banks);
    printf("\n");
    if (rangeCount < kMaxRanges) {
        ranges[rangeCount++] = {cls, name, banks};
    }
}

// The L1 block tables reachable from the MMU state, and the pages they map.
void addMmu() {
    L1Block *blocks[nrL1Blocks + 2 + 4 + 32 + 32 + nrL1Blocks];
    int count = 0;
    auto note = [&](L1Block *block) {
        if (!block) {
            return;
        }
        for (int index = 0; index < count; ++index) {
            if (blocks[index] == block) {
                return;
            }
        }
        blocks[count++] = block;
    };
    for (int index = 0; index < nrL1Blocks; ++index) note(&l1Blocks[index]);
    for (L1Block *block : mmuState.l1Blocks) note(block);
    for (L1Block *block : mmuState.basicEnL1BlockMux) note(block);
    for (L1Block *block : mmuState.osEnL1BlockMux) note(block);
    for (L1Block *block : mmuState.cartBanks) note(block);
    for (L1Block *block : mmuState.extBanks) note(block);

    uint32_t tableBanks = 0, pageBanks = 0;
    int pages = 0;
    for (int index = 0; index < count; ++index) {
        tableBanks |= banksOf((uintptr_t)blocks[index], sizeof(L1Block));
        for (uint8_t *page : blocks[index]->pages) {
            if (page) {
                pageBanks |= banksOf((uintptr_t)page, pageSize);
                ++pages;
            }
        }
    }
    printf("  %-6s %-22s %d blocks  bank ", "core1", "L1 block tables", count);
    printBanks(tableBanks);
    printf("\n  %-6s %-22s %d page pointers  bank ", "core1", "mapped pages", pages);
    printBanks(pageBanks);
    printf("\n");
    if (rangeCount + 2 <= kMaxRanges) {
        ranges[rangeCount++] = {kCore1, "L1 block tables", tableBanks};
        ranges[rangeCount++] = {kCore1, "mapped pages", pageBanks};
    }
}

}  // namespace

void reportHotDataBanks() {
    rangeCount = 0;
    printf("HOT DATA SRAM banks (32 KB, numbered as in tools/verify_iram_hotspots.py):\n");
    add(kCore1, "mmuState", (uintptr_t)&mmuState, sizeof(mmuState));
    addMmu();
    add(kCore1, "atariRam", (uintptr_t)atariRam, baseMemSz);
    add(kCore1, "d000Read", (uintptr_t)d000Read, sizeof(d000Read));
    add(kCore1, "d000Write", (uintptr_t)d000Write, sizeof(d000Write));
    add(kCore1, "lastPageOffset", (uintptr_t)lastPageOffset, sizeof(lastPageOffset));
    add(kCore1, "core1 stack", (uintptr_t)app_cpu_stack, sizeof(app_cpu_stack));
    add(kShared, "bmonArray", (uintptr_t)bmonArray, sizeof(bmonArray));
    add(kInfo, "port_IntStack", (uintptr_t)port_IntStack, CONFIG_FREERTOS_ISR_STACKSIZE);
#if CONFIG_A8BB_SIDECAR_ENABLE
    uintptr_t linkStart = 0;
    size_t linkSize = 0;
    if (sidecar.link()) {
        i2slink_memory(sidecar.link(), &linkStart, &linkSize);
    }
    add(kCore0, "i2slink", linkStart, linkSize);
#endif

    uint32_t classBanks[kClassCount] = {};
    for (int index = 0; index < rangeCount; ++index) {
        classBanks[ranges[index].cls] |= ranges[index].banks;
    }
    for (int cls = 0; cls < kClassCount; ++cls) {
        printf("  %s banks: ", kClassName[cls]);
        printBanks(classBanks[cls]);
        printf("\n");
    }

    // Shared banks: internal SRAM only (bit 31 lumps all external memory).
    constexpr uint32_t kSram = ~(1u << 31);
    int collisions = 0;
    for (int a = 0; a < rangeCount; ++a) {
        for (int b = 0; b < rangeCount; ++b) {
            const Range &x = ranges[a], &y = ranges[b];
            const bool core0VsCore1 = x.cls == kCore0 && y.cls == kCore1;
            const bool linkVsBmon = x.cls == kCore0 && y.cls == kShared &&
                                    x.name[0] == 'i';   // "i2slink"
            const uint32_t shared = x.banks & y.banks & kSram;
            if ((core0VsCore1 || linkVsBmon) && shared) {
                printf("  HOT DATA COLLISION: %s %s and %s %s share bank ", kClassName[x.cls],
                       x.name, kClassName[y.cls], y.name);
                printBanks(shared);
                printf("\n");
                ++collisions;
            }
        }
    }
    if (!collisions) {
        printf("  no hot data collisions\n");
    }
}

#endif  // CSIM
