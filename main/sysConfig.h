#pragma once

#include <string>
#include "extMem.h"

using std::string;

class SysConfig {
    DRAM_ATTR static constexpr uint16_t WD_DISABLE = 0xffff;
public:
    int runSec;
    int startupDelayMs;
    string cartImage;
    string diskSpec[8];
    string page6BinFile;
    bool fakeCio;
    const char *bootKeyboardInput;
    bool disablePbi;
    int ioTimeoutSec, wdTimeoutSec;
    int interruptStartDelay;
    uint16_t telnetServerPort;
    bool telnetServerReadonly;
    uint16_t wdMemLoc;
    volatile int interruptTicks;
    int sysMonitorSec;
    bool haltAvailable;
    int extMemSramBanks;
    int baseMemSz;
    bool enableWifi;
    int wifiConnectDelay;
    bool enableMdns;
    int serialStatusSec;
    int serialScreenDumpSec;
    bool logScreen;
    string wifiSSID;
    string wifiPassword;
    string wifiHostname;
    ExtBankPool::ExtMemConfig extMemConf;
    // AUTO memory (docs/memory-autodetect.md): boot with 64K of base RAM
    // and no board banks; after the PBI-init memory probe, autoMemConfig()
    // (pbi.cpp) shrinks the base RAM to autoMinBaseRam when native RAM
    // reaches 48K and builds extended banks (ExtBankPool AUTO), leaving at
    // least autoMinFreeSram bytes of internal RAM free.
    bool autoMem;
    int autoMinBaseRam;
    int autoMinFreeSram;
    SysConfig();
    void save();
    void load(string configName = "");
};

extern SysConfig config;
