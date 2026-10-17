#pragma once

#include <string>
#include "extMem.h"

using std::string;

#ifndef TEST_SEC
#define TEST_SEC -1
#endif

class SysConfig {
    DRAM_ATTR static constexpr uint16_t WD_DISABLE = 0xffff;
public:
    int runSec;
    string cartImage;
    string diskSpec[8];
    string page6BinFile;
    bool fakeCio;
    const char *bootKeyboardInput;
    bool disablePbi;
    int ioTimeoutSec, wdTimeoutSec;
    uint16_t wdMemLoc;
    volatile int interruptTicks;
    int sysMonitorSec;
    bool haltAvailable;
    int extMemSramBanks;
    int baseMemSz;
    bool enableWifi;
    string wifiSSID;
    string wifiPassword;
    ExtBankPool::ExtMemConfig extMemConf;
    SysConfig();
    void save();
    void load(string configName = "");
};

extern SysConfig config;
