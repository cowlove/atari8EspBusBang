#pragma once
// The host's "proc:" disk files (Sio::setProcFsNodes(), in main.cpp).
#include "diskStitchProc.h"
#include "pbi.h"
#ifndef CSIM
#include "util.h"
#endif
#include "hwType.h"
#include "sfmt.h"

static int counter = 0;
static const vector<ProcFsConnection::ProcFsFile> procFsNodes =
{
    {
        .name = "WIFI",
        .read = [](string &buf) {},
        .write = [](const string &buf) {
            if (buf == "1") {
                wifiRun();
            } else {
                // TODO figure out how to shutdown WIFI
            }
        },
    },
    {   .name = "LOG",
        .read = [](string &buf) {},
        .write = [](const string &s) { printf("LOG: '%s'\n", s.c_str()); },
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
            printf("LOG: HALT written '%s'\n", s.c_str()); 
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
