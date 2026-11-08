#pragma once
#include "esp_attr.h"
#include <inttypes.h>
#include <stdio.h>
#include <string>

using std::string;

// StructLog<T, N>: the last N T's, each with the CCOUNT delta since the
// previous entry. A fixed ring (array, head, count): add() does no
// allocation and calls nothing, so it is safe on core 0 with core 1 hot
// (IRAM callers, interrupts masked) as long as copying a T is (true for
// the plain structs; StructLog<string> is for cold paths only).
// print() runs from normal task context.
template<class T> inline void structLogPrintEntry(const T &a) {
    for(size_t i = 0; i < sizeof(a); i++) printf(DRAM_STR("%02x "), ((const uint8_t *)&a)[i]);
    printf(DRAM_STR("\n"));
}
inline void structLogPrintEntry(const string &a) {
    printf(DRAM_STR("%s\n"), a.c_str());
}
inline void structLogPrintEntry(const PbiIocb &a) {
    printf("PBI: ");
    for(size_t i = 0; i < sizeof(a); i++) printf(DRAM_STR("%02x "), ((const uint8_t *)&a)[i]);
    printf(DRAM_STR("\n"));
}

// One SIO request sidecarHost handled, as it returns. Plain bytes, so
// StructLog::add() can copy it with core 1 hot.
struct __attribute__((packed)) SidecarSioRecord {
    uint8_t unit;        // DUNIT
    uint8_t command;     // DCOMND
    uint16_t sector;     // DAUX2:DAUX1
    uint16_t addr;       // Atari buffer (DBUF)
    uint16_t len;        // bytes moved (4 for status, else DBYT; 0 if none)
    uint8_t portb;       // d000Write[0x301]: last PORTB write
    uint8_t d1ff;        // d000Write[0x1ff]: last $D1FF (PBI select) write
    uint8_t result;      // RES_FLAG_* returned
    uint8_t dstats;      // DSTATS returned
    uint8_t path;        // 'M' mapped, 'C' via pbiROM[0x400], '-' no data
    uint32_t buffer;     // where the data was read or written on the ESP
    uint8_t data[16];    // first bytes sent (writes) or returned (reads)
};
inline void structLogPrintEntry(const SidecarSioRecord &a) {
    printf(DRAM_STR("D%d: cmd %02x sec %5u buf %04x len %4u PORTB %02x D1FF %02x"
                    " res %02x DSTATS %02x %c %08" PRIx32 " :"),
           a.unit, a.command, a.sector, a.addr, a.len, a.portb, a.d1ff,
           a.result, a.dstats, a.path, a.buffer);
    for (size_t i = 0; i < sizeof(a.data); i++) printf(DRAM_STR(" %02x"), a.data[i]);
    printf(DRAM_STR("\n"));
}

#define STRUCT_LOG
#ifdef STRUCT_LOG 
template<class T, int N = 32>
struct StructLog { 
    static_assert(N > 0, "StructLog needs at least one entry");
    struct Entry {
        uint32_t tscDelta;
        T value;
    };
    Entry log[N];
    int head = 0;          // the slot add() fills next
    int count = 0;         // valid entries, at most N
    uint32_t total = 0;    // entries ever added
    uint32_t lastTsc = 0;

    inline void IRAM_ATTR add(const T &t) {
        const uint32_t tsc = XTHAL_GET_CCOUNT();
        Entry &e = log[head];
        e.tscDelta = tsc - lastTsc;
        e.value = t;
        lastTsc = tsc;
        head = head + 1 == N ? 0 : head + 1;
        if (count < N) count++;
        total++;
    }
    static inline void printEntry(const T &a) { structLogPrintEntry(a); }
    inline void print() { 
        if (total > (uint32_t)count)
            printf(DRAM_STR("(last %d of %" PRIu32 ")\n"), count, total);
        int i = head - count;
        if (i < 0) i += N;
        for(int n = 0; n < count; n++) {
            printf(DRAM_STR("%-10" PRIu32 ": "), log[i].tscDelta);
            printEntry(log[i].value);
            i = i + 1 == N ? 0 : i + 1;
        }
    }
};

#else //#ifdef STRUCT_LOG 
template<class T, int N = 32>
struct StructLog {
    inline void IRAM_ATTR add(const T &t) {} 
    static inline void printEntry(const T &a) { structLogPrintEntry(a); }
    inline void print() {}
};
#endif

struct StructLogs { 
    StructLog<AtariDCB, 200> dcb; 
    StructLog<AtariIOCB> iocb; 
    StructLog<PbiIocb, 50> pbi;
    // Requests sidecarHost handled, logged as they return (result and
    // copybuf/copylen filled in). Their DCBs go in dcb as they arrive.
    StructLog<PbiIocb, 50> sidecarPbi;
    // The same requests' PORTB/$D1FF, ESP buffer and first data bytes.
    StructLog<SidecarSioRecord, 50> sidecarSio;
    StructLog<AtariIOCB> ziocb; 
    StructLog<string> opens;
    void print() {
        printf("PBI log:\n"); pbi.print();
        printf("Sidecar PBI log (at return):\n"); sidecarPbi.print();
        printf("Sidecar SIO log (at return):\n"); sidecarSio.print();
        printf("DCB log:\n"); dcb.print();
        printf("IOCB log:\n"); iocb.print();
        printf("ZIOCB log:\n"); ziocb.print();
        printf("opened files log:\n"); opens.print();
    }
};

template<class T> struct ScopedPrintStructLog { 
    T *p;
    ScopedPrintStructLog(T *_p) : p(_p) {
        printf("--> "); StructLog<T>::printEntry(*p);
    }
    ~ScopedPrintStructLog() {
        printf("<-- "); StructLog<T>::printEntry(*p);
    }
};

extern DRAM_ATTR StructLogs *structLogs;
