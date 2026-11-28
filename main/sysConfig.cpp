#ifdef CSIM
#include "csim.h"
#else
#include "sdkconfig.h"
#endif

#include "sysConfig.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
DRAM_ATTR SysConfig config;

// HotStr's bytes: internal RAM (heap_caps_malloc, not malloc: main's
// malloc prefers PSRAM). The empty string too: a literal would be in flash.
DRAM_ATTR const char hotStrEmpty[1] = "";
void HotStr::set(const char *s) {
    char *p = nullptr;
    if (s && *s) {
        size_t n = strlen(s) + 1;
        p = (char *)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) memcpy(p, s, n);
    }
    free(p_);
    p_ = p;
}
HotStr::~HotStr() { free(p_); }

SysConfig::SysConfig() {
#ifdef RUN_SEC
    runSec = RUN_SEC;
#endif
}

const char *const SysConfig::builtinNames[] = {
    "BENCH", "FAKESIO_CSIM", "FAKESIO_CSIM_BUS", "MODES_CSIM_BUS", "SDX_CSIM", "SDX_CSIM_SMB",
    "SDX_CSIM_BUS", "BANKTEST", "BANKTEST_MC", "SDX_XL", "SDX_600XL", "SDX_XE128", "SDX_AUTO",
    "SDX_CSIM_BUS_AUTO", "SDX_XE192_SC", "SDX_XE192", "HELLO", "HELLO_CARTINT", "STRESS",
    "STRESS_MC", "HELLO_CART", "HELLO_CART_FAKECIO", "DOSX", "BUSA", "BASIC_DOS20", "CSIM",
    "XDOS", "WOLFENSTEIN", "CONAN", "TBASIC", "BASIC", "JOUST", nullptr
};

void SysConfig::load(string configName /*= ""*/)  {
    // No profile compiled in (-DBOOT_CONFIG): kFallback's profile (BASIC),
    // both halves (the boards' environment too: its timeouts, e.g. BASIC's
    // watchdog and IO timeout off), until the chosen file replaces the Atari half
    // (main.cpp, sidecarApp.cpp). (EspConfig's bare defaults, as before
    // 2026-11-26, turned the 6502 watchdog on, which SDX_AUTO has off: the
    // pool's runs ended on it.)
    bootConfig = configName;
    if (configName == "") {
        loadBuiltin(kFallback);
    } else if (!loadBuiltin(configName)) {
        printf("SysConfig: no built-in profile %s: the defaults\n", configName.c_str());
    }
#ifdef CSIM
    // CSIM_RUN_SEC=<n>: this run's length, over the build's RUN_SEC (so
    // many runs can share one binary).
    if (const char *sec = getenv("CSIM_RUN_SEC"))
        runSec = atoi(sec);
    // csim's Sidecar serves its web pages on a host port
    // (csim/a800/csim_httpd.cpp): unprivileged 8080, or CSIM_HTTP_PORT=<n>
    // (0: no web server, e.g. for parallel runs).
    httpServerPort = 8080;
    if (const char *port = getenv("CSIM_HTTP_PORT"))
        httpServerPort = atoi(port);
#endif
#ifdef CART_IMAGE
    // -DCART_IMAGE=\"<spec>\": this build's cartridge, whatever the profile
    // says (e.g. \"sidecar!/sd/SDX451_atarimax128.car.gz\"; "" none).
    cartImage = CART_IMAGE;
#endif
    loaded = true;
}

bool SysConfig::loadBuiltin(const string &configName) {
    static_cast<AtariConfig &>(*this) = AtariConfig();
    static_cast<EspConfig &>(*this) = EspConfig();
    runSec = 600;
#ifdef RUN_SEC
    runSec = RUN_SEC;
#endif


#ifndef SIDECAR_D1
#define SIDECAR_D1 "sidecar!/sd/d1.atr,direct"   // BENCH's Sidecar D1 (below)
#endif
    if (configName == "BENCH") { 
        interruptTicks = 0;
#ifdef FAKE_SIO
        // FAKE_SIO's bank ops (fakeSio.cpp): RAMBO256 with only 2 of its
        // 12 banks in SRAM, so most switches evict one to PSRAM. The halt
        // line goes nowhere on the bench, but PSRAM paging requires it.
        haltAvailable = true;
        extMemSramBanks = 2;
        extMemConf = ExtMemLayout::RAMBO256;
        // What FAKE_SIO's sector reads hit. With the Sidecar link, the
        // bench pair's Sidecar serves it from its SD card, in place
        // ("direct": every read and write goes to the card, sio.h). Put it
        // there with the Sidecar console: "cp /spiffs/d1.atr.gz /sd/d1.atr.gz"
        // (the spiffs copy comes from sidecar/flash_spiffs.sh), then
        // "gunzip /sd/d1.atr.gz /sd/d1.atr".
#if A8BB_SIDECAR
        diskSpec[0] = SIDECAR_D1;
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

    } else if (configName == "FAKESIO_CSIM" || configName == "FAKESIO_CSIM_BUS") {
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
        if (configName == "FAKESIO_CSIM_BUS") {
            // BUS_CYCLE=1: the board's RAMBO256 banks too, which FAKE_SIO
            // switches with $D301 writes of its own (fakeSio.cpp's bank ops).
            // The board serves the idle cartridge, as SDX_CSIM_BUS does
            // its SDX: atari800 gets none. (Its own cart under the board's
            // base RAM at $A000 failed the OS's cartridge check on some
            // seeds: the 6502 ran off into the display list.)
            cartImage = "idle.car";   // relative: csim/a800/, made by run_fakesio.sh
            haltAvailable = true;
            extMemSramBanks = 1;
            extMemConf = ExtMemLayout::RAMBO256;
        }

    } else if (configName == "MODES_CSIM_BUS") {
        // csim BUS_CYCLE=1: the board serves csim/a800/make_modes_cart.py's
        // cartridge (a band of each ANTIC mode, all in its ROM), for the
        // shutdown screenshot (main/screenshot.cpp, tools/screenshot_png.py).
        // The 6502 parks in RAM and never calls the PBI device.
        cartImage = "modes.car";   // relative: csim/a800/
        interruptTicks = 0;
        wdTimeoutSec = ioTimeoutSec = -1;

    } else if (configName == "SDX_CSIM") {
        diskSpec[0] = "sidecar!/d1.atr.gz";
        diskSpec[1] = "sidecar!/d2.atr.gz";
        diskSpec[2] = "sidecar!proc:";
        //diskSpec[3] = "smb://miner6.local/pub";
	    diskSpec[3] = "sidecar!spiffs:./fs/";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        wdTimeoutSec = -1;
    
    } else if (configName == "SDX_CSIM_SMB") {
        // SDX_CSIM with D4 (X.BAT's SMB copies, FOO to FOO2 to FOO3) an SMB
        // stitch on the Sidecar, through a local TCP proxy to miner6's share,
        // in a directory of its own (holding a FOO):
        // csim/a800/smb_outage_test.sh stops and kills the proxy to test the
        // connection's recovery (diskStitchSmb.h).
        diskSpec[0] = "sidecar!/d1.atr.gz";
        diskSpec[1] = "sidecar!/d2.atr.gz";
        diskSpec[2] = "sidecar!proc:";
        diskSpec[3] = "sidecar!smb://127.0.0.1:4450/pub/csim-smbtest";
        bootKeyboardInput = DRAM_STR("-2:X\233");
        wdTimeoutSec = -1;

    } else if (configName == "SDX_CSIM_BUS") {
        // SDX_CSIM for csim's BUS_CYCLE=1 mode: the board, not atari800,
        // supplies the SDX cartridge and RAMBO256 extended memory.
        diskSpec[0] = "sidecar!/d1.atr.gz";
        diskSpec[1] = "sidecar!/d2.atr.gz";
        diskSpec[2] = "sidecar!proc:";
        diskSpec[3] = "sidecar!spiffs:./fs/";
        cartImage   = "/SDX450_maxflash1.car.gz";   // spiffs/ has only the .gz
        bootKeyboardInput = DRAM_STR("-2:X\233");
        wdTimeoutSec = -1;
        haltAvailable = true;   // csim swaps PSRAM banks synchronously
        extMemSramBanks = 1;
        extMemConf = ExtMemLayout::RAMBO256;

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
        extMemConf = ExtMemLayout::RAMBO256;

    } else if (configName == "SDX_XL") { 
        diskSpec[0] = "/d1.atr.gz";   // exp-d5latch: the DOS 2 disk that hung SDX before the $D5xx latch fix
        diskSpec[1] = "/d2.atr.gz";
        diskSpec[2] = "proc:";
        diskSpec[3] = "smb://miner6.local/pub";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        wdTimeoutSec = -1;
        haltAvailable = true;
        extMemSramBanks = 1;
        extMemConf = ExtMemLayout::RAMBO256;

    } else if (configName == "SDX_600XL") { 
        enableMdns = true;
        wifiConnectDelay = 3;
        diskSpec[0] = "/d1.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        diskSpec[2] = "proc:";
        diskSpec[3] = "smb://miner6.local/pub";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("POKE 559,0\233 -2:X\233");     
        wdTimeoutSec = -1;
        haltAvailable = true;
        extMemSramBanks = 1;
        extMemConf = ExtMemLayout::RAMBO256;
        telnetServerReadonly = true;
        //interruptTicks = -1;
        // baseMemSize of 0x5c00 breaks SDX   
        // 
        //baseMemSz = 0x5c00;//32 * 1024;
        baseMemSz = 64 * 1024;
        //enableWifi = false;
    } else if (configName == "SDX_XE128") { 
        diskSpec[0] = "/d1.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        diskSpec[2] = "proc:";
        diskSpec[3] = "smb://miner6.local/pub";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        //interruptTicks = -1;
        wdTimeoutSec = -1;
        haltAvailable = false;
        extMemSramBanks = 0;
        extMemConf = ExtMemLayout::NONE;

    } else if (configName == "SDX_AUTO" || configName == "SDX_CSIM_BUS_AUTO") {
        // One SDX config for any machine: the memory layout comes from the
        // PBI-init memory probe (autoMemConfig(), pbi.cpp). Boots with 64K
        // of base RAM, no board banks and the halt line unused; the probe's
        // report sets all three. SDX_CSIM_BUS_AUTO: csim's disks and cart.
        if (configName == "SDX_AUTO") {
#if A8BB_SIDECAR   // D4 the Sidecar serves (main has no SMB)
            // D1/D2 from main's own spiffs in Sidecar builds too (2026-11-28):
            // the whole pool exercises main's local disk path (first-use
            // opens in handlePbiRequestCold(), with core 1 stalled).
            diskSpec[0] = "/d1.atr.gz";
            diskSpec[1] = "/d2.atr.gz";
            diskSpec[2] = "proc:";
            diskSpec[3] = "sidecar!smb://miner6.local/pub";
#else
            diskSpec[0] = "/d1.atr.gz";
            diskSpec[1] = "/d2.atr.gz";
            diskSpec[2] = "proc:";
            diskSpec[3] = "smb://miner6.local/pub";
#endif
            cartImage   = "/SDX450_maxflash1.car.gz";
        } else {
            diskSpec[0] = "sidecar!/d1.atr.gz";
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
        extMemConf = ExtMemLayout::NONE;
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
        diskSpec[0] = "/d1.atr.gz";
        diskSpec[1] = "sidecar!/d2.atr.gz";
        diskSpec[2] = "proc:";
        diskSpec[3] = "smb://miner6.local/pub";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        interruptTicks = 240 * 1001 * 1001 / 1;  // 1Hz
        wdTimeoutSec = -1;
        ioTimeoutSec = 30;
        haltAvailable = false;
        extMemSramBanks = 4;
        extMemConf = ExtMemLayout::NATIVE_XE_COMPY192;

    } else if (configName == "SDX_XE192") { 
        diskSpec[0] = "/d1.atr.gz";
        diskSpec[1] = "/d2.atr.gz";
        diskSpec[2] = "proc:";
        diskSpec[3] = "smb://miner6.local/pub";
        cartImage   = "/SDX450_maxflash1.car.gz";
        bootKeyboardInput = DRAM_STR("-2:X\233");     
        //interruptTicks = -1;
        wdTimeoutSec = -1;
        haltAvailable = false;
        extMemSramBanks = 4;
        extMemConf = ExtMemLayout::NATIVE_XE_COMPY192;

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
        extMemConf = ExtMemLayout::NONE;

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
        extMemConf = ExtMemLayout::XE;

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

    } else if (configName == "XDOS") { 
        diskSpec[0] = "/xdos251_tbasic.atr.gz";
        bootKeyboardInput = "DOS\233";
        baseMemSz = 64 * 1024;
        interruptTicks = 0;

    } else if (configName == "WOLFENSTEIN") { 
        diskSpec[0] = "/Castle_Wolfenstein.atr.gz";
        baseMemSz = 64 * 1024;
        interruptTicks = 0;
    
    } else if (configName == "CONAN") { 
        diskSpec[0] = "/conan1.atr.gz";
        diskSpec[1] = "/conan2.atr.gz";
        baseMemSz = 64 * 1024;
        interruptTicks = 0;
        diskIdleSwapDelay = 3;
        diskIdleSwapRepeat = 0;
        diskIdleSwapKeys = DRAM_STR(" ");

    } else if (configName == "TBASIC") { 
        diskSpec[0] = "/tbasic.atr.gz";
        bootKeyboardInput = "DIR\233PAUSE 1\233";
        baseMemSz = 64 * 1024;
        interruptTicks = 0;

    } else if (configName == "BASIC") { 
        diskSpec[0] = "/dos20boot.atr.gz";
        enableBasic = true;
        baseMemSz = 64 * 1024;
        bootKeyboardInput = DRAM_STR(
            "10 PRINT \"HELLO-> \"; \233"
          //  "20 POKE 1536, 222 \233"
          //  "30 GOTO 10 \233"
            "\233"
            "LIST \"J1:\\LOG\"\233"
            "RUN\233"
        );     
    	wdTimeoutSec = ioTimeoutSec = -1;

    } else if (configName == "JOUST") { 
        cartImage   = "/Joust.rom.gz";
        interruptTicks = 0;;//240 * 1000 * 1000 * 1;
    	wdTimeoutSec = ioTimeoutSec = 0;
        runSec = -1;
        baseMemSz = 48 * 1024;
    } else {
        return false;
    }
    return true;
}
