#pragma once
// The host's "proc:" disk files (Sio::setProcFsNodes(), in main.cpp).
#include "a8log.h"
#include "diskStitchProc.h"
#include "pbi.h"
#include "cio.h"
#ifndef CSIM
#include "util.h"
#include "esp_mac.h"   // esp_read_mac() (MAC node); csim stubs it (csim.h)
#endif
#include "hwType.h"
#include "sfmt.h"

static int counter = 0;
static vector<ProcFsConnection::ProcFsFile> procFsNodes =
{
    {   .name = "LOG",
        .read = [](string &buf) {},
        .write = [](const string &s) { LOG("LOG: '%s'\n", s.c_str()); },
    },
    {   .name = "UPTIME",
        .read = [](string &buf) { buf = sfmt("%d\233", elapsedSec); },
    },
    {   .name = "COUNTER",
        .read = [](string &buf) { buf = sfmt("%d\233", ++counter); },
        .write = [](const string &s) { counter = 0; },
    },
    {   .name = "SCREEN",
        .write = [](const string &s) { dumpScreenToSerial('S'); }
    },
    {   .name = "HALT",
        .write = [](const string &s) { 
            LOG("LOG: HALT written '%s'\n", s.c_str()); 
            core0_exit(-5, "ProcFS HALT file written :%s", s.c_str());
		    dumpScreenToSerial('H');
        },
    },
    {
        .name = "MAC",
        .read = [](string &buf) {
            uint8_t chipid[6];
            esp_read_mac(chipid, ESP_MAC_WIFI_STA);
            buf = sfmt("%02X%02X%02X%02X%02X%02X\233",chipid[0], chipid[1], 
                    chipid[2], chipid[3], chipid[4], chipid[5]);
        },
    },
    {
        .name = "HWTYPE",
        .read = [](string &s) { s = HW_TYPE; } 
    },
    {
        .name = "GIT",
        .read = [](string &s) { s = GIT_VERSION; } 
    },
};


class ProcFileHandler final : public CioFileHandler {
    ProcFsConnection::ProcFsFile *file;
public:
    ProcFileHandler(ProcFsConnection::ProcFsFile *f) : file(f) {}

    bool open(PbiIocb *, const AtariIOCB *iocb, uint8_t, const std::string &) override {
        if (file == nullptr) return false;
        buf = "";
        writeOpen = iocb->ICAX1 & 0x8;
        if (iocb->ICAX1 & 0x4) 
            file->read(buf); 
        return true;
    }

    void close(PbiIocb *, uint8_t) override {
        if (file != nullptr && writeOpen) file->write(buf);
    }

    int get(PbiIocb *, uint8_t) override {
        if (buf.length() == 0)  
            return -1;
        int rval = buf[0];
        buf.erase(0, 1);
        return rval;
    }

    bool put(PbiIocb *, uint8_t, uint8_t c) override {
        if (c != '\n' && c != 155)
            buf += (char)c;
        return true;
    }

private:
    bool writeOpen;
    string buf;
};

static void initProcFsNodes() { 
    for(auto &n : procFsNodes) {
        for(const string prefix : {"J:", "J:\\", "J1:", "J1:\\"}) {  
            string fn = prefix + n.name;
            registerCioFileHandler(fn.c_str(), [p = &n]() { 
                return (CioFileHandler *)new (std::nothrow)ProcFileHandler(p); 
            });
        }
    }
}