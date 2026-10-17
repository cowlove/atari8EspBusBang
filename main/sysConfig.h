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
    int runSec = TEST_SEC;
    string cartImage;
    string diskSpec[8];
    string page6BinFile;
    bool fakeCio = false;
    const char *bootKeyboardInput = "";
    bool disablePbi = false;
    int ioTimeoutSec = 120, wdTimeoutSec = 120;
    uint16_t wdMemLoc = WD_DISABLE;
    volatile int interruptTicks = 240 * 1001 * 1001 / 5;  // 5Hz
    int sysMonitorSec = 0;
    bool haltAvailable = false;
    int extMemSramBanks = 0;
    int baseMemSz = 2 * 1024;
    bool enableWifi = true;
    ExtBankPool::ExtMemConfig extMemConf = ExtBankPool::ExtMemConfig::NONE;
    void save();
    void load(string configName = "");
};

extern SysConfig config;
