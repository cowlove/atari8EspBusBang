#ifdef CSIM
#include "csim.h"
#else
#include "sdkconfig.h"   // CONFIG_A8BB_SIDECAR_ENABLE
#endif

#include "sysConfig.h"
#include "asmDefs.h"
#include "hostSio.h"

#include <fcntl.h>
#include <unistd.h>
DRAM_ATTR SysConfig config;

SysConfig::SysConfig() {
#ifdef RUN_SEC
    runSec = RUN_SEC;
#else
    runSec = -1;
#endif
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
    serialStatusSec = 2;
    serialScreenDumpSec = 90;
    logScreen = true;
    wifiSSID = "ClemmyNet";
    wifiPassword = "clementine is a cat";
    wifiHostname = "busbang-$MAC";
    extMemConf = ExtBankPool::ExtMemConfig::NONE;
    autoMem = false;
    autoMinBaseRam = 2 * 1024;
    autoMinFreeSram = 16 * 1024;
}

void SysConfig::load(string configName /*= ""*/)  {
#if 0    
    int fd = ::open(sio.resolve("/config.txt").c_str(), O_RDONLY);
    char buf[128] = {0};
    if (fd >= 0) {
        ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
    }
    cartImage = buf;
#endif

    runSec = 600;
#ifdef RUN_SEC
    runSec = RUN_SEC;
#endif
    if (configName == "") configName = "STRESS";

    if (configName == "BENCH") { 
        interruptTicks = 0;
#ifdef FAKE_SIO
        // What FAKE_SIO's sector reads hit. With the Sidecar link, the
        // bench pair's Sidecar serves it (its spiffs, sidecar/flash_spiffs.sh),
        // so the reads exercise the Sidecar's SIO and its .atr.gz inflate.
#if CONFIG_A8BB_SIDECAR_ENABLE
        diskSpec[0] = "sidecar!/d1.atr.gz";
        // And a local D2: FAKE_SIO alternates D1/D2, so the local requests
        // (which detach the bus) print the io: lines and read serial input.
        diskSpec[1] = "/d1.atr.gz";
#else
        diskSpec[0] = "/d1.atr.gz";
#endif
#endif
#ifdef RUN_SEC
        runSec = RUN_SEC;
#else
        runSec = -1;
#endif

    } else if (configName == "FAKESIO_CSIM") {
        // csim's FAKE_SIO stress test (make FAKE_SIO=1, csim/a800/run_fakesio.sh):
        // the bench's disks, D1: the Sidecar's and D2: local, the same
        // image. atari800 runs an idle cartridge (csim/a800/make_idle_cart.py),
        // and no PBI interrupts, so the 6502 never touches the PBI device
        // after its boot-time init and fakeSioPoll() has the request block
        // to itself.
        diskSpec[0] = "sidecar!/d1.atr.gz";
        diskSpec[1] = "/d1.atr.gz";
        interruptTicks = 0;
        wdTimeoutSec = ioTimeoutSec = -1;

    } else if (configName == "SDX_CSIM") { 
        diskSpec[0] = "sidecar!/toolkit.atr.gz";
        diskSpec[1] = "sidecar!/d2.atr.gz";
        diskSpec[2] = "sidecar!proc:";
        //diskSpec[3] = "smb://miner6.local/pub";
	    diskSpec[3] = "sidecar!spiffs:./fs/";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        wdTimeoutSec = -1;
    
    } else if (configName == "SDX_CSIM_BUS") {
        // SDX_CSIM for csim's BUS_CYCLE=1 mode: the board, not atari800,
        // supplies the SDX cartridge and RAMBO256 extended memory.
        diskSpec[0] = "sidecar!/toolkit.atr.gz";
        diskSpec[1] = "sidecar!/d2.atr.gz";
        diskSpec[2] = "sidecar!proc:";
        diskSpec[3] = "sidecar!spiffs:./fs/";
        cartImage   = "/SDX450_maxflash1.car.gz";   // spiffs/ has only the .gz
        bootKeyboardInput = DRAM_STR("-2:X\233");
        wdTimeoutSec = -1;
        haltAvailable = true;   // csim swaps PSRAM banks synchronously
        extMemSramBanks = 1;
        extMemConf = ExtBankPool::ExtMemConfig::RAMBO256;

    } else if (configName == "BANKTEST" || configName == "BANKTEST_MC") {
        // spiffs/stress.c's bank tests: does a PORTB, $D5xx or $D1FF bank
        // switch reach the very next bus cycle? (stress's other tests run
        // too.) No disks; it reports through J1: calls. BANKTEST_MC is the
        // MegaCart build, whose bank select is the data written to $D500.
        cartImage   = configName == "BANKTEST" ? "/stress.car.gz" : "/stress-megacart.car.gz";
        wdTimeoutSec = -1;
        // One SRAM bank for its two: every switch evicts, so the 6502 halts
        // on the STA PORTB while the bank is paged in from PSRAM and the MMU
        // is patched. That transition is part of what it tests. PSRAM-backed
        // banks need the halt signal.
        haltAvailable = true;
        extMemSramBanks = 1;
        extMemConf = ExtBankPool::ExtMemConfig::RAMBO256;

    } else if (configName == "SDX_XL") { 
        diskSpec[0] = "/toolkit.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        diskSpec[2] = "proc:";
        diskSpec[3] = "smb://miner6.local/pub";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        wdTimeoutSec = -1;
        haltAvailable = true;
        extMemSramBanks = 1;
        extMemConf = ExtBankPool::ExtMemConfig::RAMBO256;

    } else if (configName == "SDX_600XL") { 
        enableMdns = true;
        wifiConnectDelay = 3;
        diskSpec[0] = "/toolkit.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        diskSpec[2] = "proc:";
        diskSpec[3] = "smb://miner6.local/pub";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("POKE 559,0\233 -2:X\233");     
        wdTimeoutSec = -1;
        haltAvailable = true;
        extMemSramBanks = 1;
        extMemConf = ExtBankPool::ExtMemConfig::RAMBO256;
        telnetServerReadonly = true;
        //interruptTicks = -1;
        // baseMemSize of 0x5c00 breaks SDX   
        // 
        //baseMemSz = 0x5c00;//32 * 1024;
        baseMemSz = 64 * 1024;
        //enableWifi = false;
    } else if (configName == "SDX_XE128") { 
        diskSpec[0] = "/toolkit.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        diskSpec[2] = "proc:";
        diskSpec[3] = "smb://miner6.local/pub";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        //interruptTicks = -1;
        wdTimeoutSec = -1;
        haltAvailable = false;
        extMemSramBanks = 0;
        extMemConf = ExtBankPool::ExtMemConfig::NONE;

    } else if (configName == "SDX_AUTO" || configName == "SDX_CSIM_BUS_AUTO") {
        // One SDX config for any machine: the memory layout comes from the
        // PBI-init memory probe (autoMemConfig(), pbi.cpp). Boots with 64K
        // of base RAM, no board banks and the halt line unused; the probe's
        // report sets all three. SDX_CSIM_BUS_AUTO: csim's disks and cart.
        if (configName == "SDX_AUTO") {
            diskSpec[0] = "/toolkit.atr.gz";
            diskSpec[1] = "/d2.atr.gz";
            diskSpec[2] = "proc:";
            diskSpec[3] = "smb://miner6.local/pub";
            cartImage   = "/SDX450_maxflash1.car.gz";
        } else {
            diskSpec[0] = "sidecar!/toolkit.atr.gz";
            diskSpec[1] = "sidecar!/d2.atr.gz";
            diskSpec[2] = "sidecar!proc:";
            diskSpec[3] = "sidecar!spiffs:./fs/";
            cartImage   = "/SDX450_maxflash1.car.gz";
        }
        bootKeyboardInput = DRAM_STR("-2:X\233");
        wdTimeoutSec = -1;
        baseMemSz = 64 * 1024;
        haltAvailable = false;
        extMemSramBanks = 0;
        extMemConf = ExtBankPool::ExtMemConfig::NONE;
        autoMem = true;
        autoMinBaseRam = 2 * 1024;
        // Internal RAM left free after the banks: WiFi and breathing room.
        // Today's configs boot with 33-39K free on a 130XE (SDX_XE192), 51K
        // on a 600XL (SDX_600XL), 79K on an 800XL (SDX_XL), WiFi fine on all;
        // the 600XL's WiFi start failed at 17.8K (7.7K largest block). 16K
        // (user, 2026-11-12): sure to boot; if WiFi crashes out later, we'll
        // know. A 130XE keeps 4 banks; a 600XL gets 3 SRAM fronts, ~19K left.
        autoMinFreeSram = 16 * 1024;

    } else if (configName == "SDX_XE192_SC") { 
        diskSpec[0] = "/toolkit.atr.gz";
        diskSpec[1] = "sidecar!/d2.atr";
        diskSpec[2] = "proc:";
        diskSpec[3] = "smb://miner6.local/pub";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        interruptTicks = 240 * 1001 * 1001 / 1;  // 1Hz
        wdTimeoutSec = -1;
        ioTimeoutSec = 30;
        haltAvailable = false;
        extMemSramBanks = 4;
        extMemConf = ExtBankPool::ExtMemConfig::NATIVE_XE_COMPY192;

    } else if (configName == "SDX_XE192") { 
        diskSpec[0] = "/toolkit.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        diskSpec[2] = "proc:";
        diskSpec[3] = "smb://miner6.local/pub";
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
        wifiConnectDelay = 10;
        cartImage   = "/stress.car.gz";
        diskSpec[0] = "/dos20boot.atr.gz";
        interruptTicks = 240 * 1001 * 1001 / 10;
        wdTimeoutSec = 0;
        ioTimeoutSec = 30;
        //fakeCio = true;
        baseMemSz = 16 * 1024;
        
        // TODO: even just haltAvailable doesn't boot on 600XL
        haltAvailable = false;
        extMemSramBanks = 1;
        extMemConf = ExtBankPool::ExtMemConfig::NONE;

    } else if(configName == "STRESS_MC") {
        //startupDelayMs = 3000;
        wifiConnectDelay = 10;
        cartImage   = "/stress-megacart.car.gz";
        diskSpec[0] = "/dos20boot.atr.gz";
        interruptTicks = 240 * 1001 * 1001 / 25;
        wdTimeoutSec = 0;
        ioTimeoutSec = 120;
        //fakeCio = true;
#if 1
        baseMemSz = 64 * 1024;
        haltAvailable = true;
        extMemSramBanks = 1;
#else 
        baseMemSz = 32 * 1024;
        haltAvailable = false;
        extMemSramBanks = 4;
#endif
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

    } else if (configName == "BASIC_DOS20") { 
       /* bootKeyboardInput = DRAM_STR(
            "10 PRINT \"HELLO-> \"; \233"
            "20 POKE 1536, 222 \233"
            "30 GOTO 10 \233"
            "\233"
        );     */
        wifiConnectDelay = 3;
        diskSpec[0] = "/dos20d.atr";
        diskSpec[1] = "smb://miner6.local/pub";
        diskSpec[2] = "proc:";
       interruptTicks = 240 * 1001 * 1000 / 20 ;
    	wdTimeoutSec = ioTimeoutSec = -50;

    } else if (configName == "CSIM") {
        serialStatusSec = 300;
        serialScreenDumpSec = -1;
        //wifiConnectDelay = 3;
        diskSpec[0] = "./d1_test.atr";
        //diskSpec[0] = "./d1.atr";
        diskSpec[1] = "./sdx-toolkit.atr";
        diskSpec[2] = "proc:";
	//diskSpec[3] is hdd image
	diskSpec[4] = "spiffs:./fs/";
        diskSpec[5] = "sidecar!/d2.atr.gz";   // test: D6: mounted on the Sidecar
        diskSpec[7] = "./SDX_hdd.img";
        interruptTicks = 240 * 1001 * 1000 / 25;
        //interruptTicks  = -1;
        //bootKeyboardInput = "HELLO \233";
        wdTimeoutSec = -1;
        ioTimeoutSec = -2;
        baseMemSz = 64 * 1024;

    } else if (configName == "BASIC") { 
        bootKeyboardInput = DRAM_STR(
            "10 PRINT \"HELLO-> \"; \233"
            "20 POKE 1536, 222 \233"
            "30 GOTO 10 \233"
            "\233"
        );     
        //interruptTicks = 240 * 1000 * 1000 * 1;
    	wdTimeoutSec = ioTimeoutSec = -1;

    } else if (configName == "JOUST") { 
        cartImage   = "/Joust.rom.gz";
        interruptTicks = 0;;//240 * 1000 * 1000 * 1;
    	wdTimeoutSec = ioTimeoutSec = 0;
        runSec = -1;
    }
#ifdef CSIM
    // CSIM_RUN_SEC=<n>: this run's length, over the build's RUN_SEC (so
    // many runs can share one binary).
    if (const char *sec = getenv("CSIM_RUN_SEC"))
        runSec = atoi(sec);
#endif
}

void SysConfig::save() {
    int fd = ::open(sio.resolve("/config.txt").c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return;
    if (::write(fd, cartImage.c_str(), cartImage.length()) < 0) {}   // best effort
    ::close(fd);
}
