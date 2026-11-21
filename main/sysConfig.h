#pragma once

// Shared by both apps: main and the Sidecar (sidecar/main/CMakeLists.txt)
// build sysConfig.cpp, and both load the same BOOT_CONFIG profile.
#include <stdint.h>
#include <string>
#include "esp_attr.h"
#include "extMemConfig.h"

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
    uint16_t httpServerPort;   // the Sidecar's web server; 0: none
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
    ExtMemLayout::ExtMemConfig extMemConf;
    // AUTO memory (docs/memory-autodetect.md): boot with 64K of base RAM
    // and no board banks; after the PBI-init memory probe, autoMemConfig()
    // (pbi.cpp) shrinks the base RAM to autoMinBaseRam when native RAM
    // reaches 48K and builds extended banks (ExtBankPool AUTO), leaving at
    // least autoMinFreeSram bytes of internal RAM free.
    bool autoMem;
    int autoMinBaseRam;
    int autoMinFreeSram;
    bool loaded = false;   // load() has run (csim: the Sidecar's coroutine waits for main's)
    SysConfig();
    void save();   // main only (main.cpp): the cart choice, to main's spiffs
    void load(string configName = "");
};

extern SysConfig config;
