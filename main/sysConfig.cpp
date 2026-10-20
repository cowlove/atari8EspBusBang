#include "sysConfig.h"
#include "asmDefs.h"
#include "spiffs.h"

struct spiffs_t;
extern struct spiffs_t *spiffs_fs;
DRAM_ATTR SysConfig config;

SysConfig::SysConfig() {
    runSec = TEST_SEC;
    startupDelayMs = 0;
    cartImage = "";
    for (string &disk : diskSpec) disk = "";
    page6BinFile = "";
    fakeCio = false;
    bootKeyboardInput = "";
    disablePbi = false;
    ioTimeoutSec = wdTimeoutSec = 120;
    interruptStartDelay = 10;
    telnetServerPort = 23;
    telnetServerReadonly = false;
    wdMemLoc = WD_DISABLE;
    interruptTicks = 240 * 1001 * 1001 / 5;  // 5Hz
    sysMonitorSec = 0;
    haltAvailable = false;
    extMemSramBanks = 0;
    baseMemSz = 2 * 1024;
    enableWifi = true;
    wifiConnectDelay = 0;
    enableMdns = false;
    wifiSSID = "ClemmyNet";
    wifiPassword = "clementine is a cat";
    wifiHostname = "busbang-$MAC";
    extMemConf = ExtBankPool::ExtMemConfig::NONE;
}

void SysConfig::load(string configName /*= ""*/)  {
#if 0    
    spiffs_file fd = SPIFFS_open(spiffs_fs, "/config.txt", SPIFFS_O_RDONLY, 0);
    char buf[128] = {0};
    SPIFFS_read(spiffs_fs, fd, buf, sizeof(buf));
    cartImage = buf;
    SPIFFS_close(spiffs_fs, fd);
#endif

    runSec = 600;
#ifdef RUNTIME_SEC
    runSec = RUNTIME_SEC;
#endif
    if (configName == "") configName = "STRESS";

    if (configName == "BENCH") { 
        interruptTicks = 0;
        runSec = TEST_SEC;

    } else if (configName == "SDX_XL") { 
        diskSpec[0] = "/toolkit.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        wdTimeoutSec = -1;
        haltAvailable = true;
        extMemSramBanks = 1;
        extMemConf = ExtBankPool::ExtMemConfig::RAMBO256;

    } else if (configName == "SDX_600XL") { 
        enableMdns = true;
        diskSpec[0] = "/toolkit.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("POKE 559,0\233 -2:X\233");     
        wdTimeoutSec = -1;
        haltAvailable = true;
        extMemSramBanks = 1;
        extMemConf = ExtBankPool::ExtMemConfig::RAMBO256;
        //interruptTicks = -1;
        // baseMemSize of 0x5c00 breaks SDX   
        // 
        //baseMemSz = 0x5c00;//32 * 1024;
        baseMemSz = 64 * 1024;
        //enableWifi = false;
        //diskSpec[2] = "/d2.atr"; // tmp block out SMB disk thats hardcoded in main.cpp

    } else if (configName == "SDX_XE128") { 
        diskSpec[0] = "/toolkit.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        //interruptTicks = -1;
        wdTimeoutSec = -1;
        haltAvailable = false;
        extMemSramBanks = 0;
        extMemConf = ExtBankPool::ExtMemConfig::NONE;

    } else if (configName == "SDX_XE192") { 
        diskSpec[0] = "/toolkit.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        //interruptTicks = -1;
        wdTimeoutSec = -1;
        haltAvailable = false;
        extMemSramBanks = 4;
        extMemConf = ExtBankPool::ExtMemConfig::NATIVE_XE_COMPY192;

    } else if(configName == "HELLO") {
        diskSpec[0] = "/llvm_d1.atr.gz";
        //interruptTicks = -1;
        wdTimeoutSec = ioTimeoutSec = 10;
        baseMemSz = 64 * 1024;


    } else if(configName == "HELLO_CARTINT") {
        cartImage   = "/hello.rom.gz";
        //interruptTicks = 0;
        baseMemSz = 64 * 1024;

    } else if(configName == "STRESS") {
        //startupDelayMs = 3000;
        //wifiConnectDelay = 10;
        cartImage   = "/stress.car.gz";
        diskSpec[0] = "/dos20boot.atr.gz";
        interruptTicks = 240 * 1001 * 1001 / 10;
        wdTimeoutSec = 0;
        ioTimeoutSec = 30;
        //fakeCio = true;
        baseMemSz = 64 * 1024;
        
        // TODO: even just haltAvailable doesn't boot on 600XL
        haltAvailable = true;
        extMemSramBanks = 1;
        extMemConf = ExtBankPool::ExtMemConfig::RAMBO256;

    } else if(configName == "STRESS_MC") {
        //startupDelayMs = 3000;
        //wifiConnectDelay = 10;
        cartImage   = "/stress-megacart.car.gz";
        diskSpec[0] = "/dos20boot.atr.gz";
        interruptTicks = 240 * 1001 * 1001 / 25;
        wdTimeoutSec = 0;
        ioTimeoutSec = 120;
        //fakeCio = true;
        baseMemSz = 32 * 1024;

        // TODO: even just haltAvailable doesn't boot on 600XL
        haltAvailable = false;//true;
        extMemSramBanks = 4;
        extMemConf = ExtBankPool::ExtMemConfig::XE;

    } else if(configName == "HELLO_CART") {
        cartImage   = "/hello.rom.gz";
        interruptTicks = 0;
        baseMemSz = 64 * 1024;

    } else if(configName == "HELLO_CART_FAKECIO") {
        cartImage   = "/hello.rom.gz";
        interruptTicks = 0;
        fakeCio = true;
        haltAvailable = true;

    } else if(configName == "DOSX") { 
        diskSpec[0] = "/d1.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        bootKeyboardInput = DRAM_STR("PAUSE 1\233E.\"J:X\"\233");
    } else if (configName == "BUSA") { 
        cartImage   = "/busa.rom.gz";
        //interruptTicks = -1;       

    } else if (configName == "BASIC_DOS25") { 
        bootKeyboardInput = DRAM_STR(
            "10 PRINT \"HELLO-> \"; \233"
            "20 POKE 1536, 222 \233"
            "30 GOTO 10 \233"
            "RUN\233"
        );     
        diskSpec[0] = "/dos25.atr.gz";
        interruptTicks = 240 * 1000 * 1000 * 1;
    	wdTimeoutSec = ioTimeoutSec = 50;

    } else if (configName == "BASIC") { 
        bootKeyboardInput = DRAM_STR(
            "10 PRINT \"HELLO-> \"; \233"
            "20 POKE 1536, 222 \233"
            "30 GOTO 10 \233"
            "RUN\233"
        );     
        interruptTicks = 240 * 1000 * 1000 * 1;
    	wdTimeoutSec = ioTimeoutSec = 50;

    } else if (configName == "JOUST") { 
        cartImage   = "/Joust.rom.gz";
        interruptTicks = 0;;//240 * 1000 * 1000 * 1;
    	wdTimeoutSec = ioTimeoutSec = 0;
        runSec = -1;
    }
}

void SysConfig::save() {
    spiffs_file fd = SPIFFS_open(spiffs_fs, "/config.txt", SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_RDWR, 0);
    SPIFFS_write(spiffs_fs, fd, (void *)cartImage.c_str(), cartImage.length());
    SPIFFS_close(spiffs_fs, fd);
}
