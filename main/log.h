#pragma once
#include "esp_attr.h"
#include <inttypes.h>
#include <string>

using std::string;

#define STRUCT_LOG
#ifdef STRUCT_LOG 
template<class T> 
struct StructLog { 
    int maxSize;
    uint32_t lastTsc;
    StructLog(int maxS = 32) : maxSize(maxS) {}
    std::deque<std::pair<uint32_t,T>> log;
    inline void IRAM_ATTR add(const T &t) {
        uint32_t tsc = XTHAL_GET_CCOUNT(); 
        log.push_back(std::pair<uint32_t,T>(tsc - lastTsc, t));
        lastTsc = tsc;
        if (log.size() > maxSize) log.pop_front();
    }
    static inline /*IRAM_ATTR*/ void printEntry(const T&);
    inline void /*IRAM_ATTR*/ print() { 
        for(auto a : log) {
            printf(DRAM_STR("%-10" PRIu32 ": "), a.first);
            printEntry(a.second);
        } 
    }
};
template <class T> inline /*IRAM_ATTR*/ void StructLog<T>::printEntry(const T &a) {
    for(int i = 0; i < sizeof(a); i++) printf(DRAM_STR("%02x "), ((uint8_t *)&a)[i]);
    printf(DRAM_STR("\n"));
}
template <> inline void StructLog<string>::printEntry(const string &a) { 
    printf(DRAM_STR("%s\n"), a.c_str()); 
}
#else //#ifdef STRUCT_LOG 
template<class T> 
struct StructLog {
    StructLog(int maxS = 32) {}
    inline void IRAM_ATTR add(const T &t) {} 
    static inline IRAM_ATTR void  printEntry(const T&) {}
    inline void IRAM_ATTR print() {}
};
#endif

extern struct StructLogs { 
    StructLog<AtariDCB> dcb = StructLog<AtariDCB>(200); 
    StructLog<AtariIOCB> iocb; 
    StructLog<PbiIocb> pbi = StructLog<PbiIocb>(50);
    StructLog<AtariIOCB> ziocb; 
    StructLog<string> opens;
    void print() {
        printf("PBI log:\n"); pbi.print();
        printf("DCB log:\n"); dcb.print();
        printf("IOCB log:\n"); iocb.print();
        printf("ZIOCB log:\n"); ziocb.print();
        printf("opened files log:\n"); opens.print();
    }
} *structLogs;

