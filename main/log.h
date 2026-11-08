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
    StructLog<AtariIOCB> ziocb; 
    StructLog<string> opens;
    void print() {
        printf("PBI log:\n"); pbi.print();
        printf("Sidecar PBI log (at return):\n"); sidecarPbi.print();
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
