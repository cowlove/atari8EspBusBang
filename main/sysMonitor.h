#pragma once
// The system monitor: the configuration menu the PBI driver shows while the
// Atari keeps re-issuing PBI command 11 (about 50 Hz), one SysMonitor::pbi()
// pass per command, drawn straight into the Atari's screen memory.
//
// pbi() is hot-safe: it can run while core 1 drives the bus. The menu classes
// (sysMonitor.cpp) use ETL's fixed-capacity strings and vectors instead of
// std::, live in internal RAM, and their code and constants are placed in
// internal RAM (main/link.lf), so the hot path touches no flash, PSRAM or
// peripheral. What can't run hot (saving config, opening a cartridge) is
// queued and done by runColdWork() on a cold call (sysMonitorCold.cpp).
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <functional>
#include "pbi.h"
using std::string;
using std::function;
using std::min;
using std::max;
using std::vector;

#include "sfmt.h"

#ifndef CSIM
#include "xtensa/core-macros.h"
#endif

class SysMonitorMenu;

struct Debounce {
    int last = 0;
    int stableTime = 0;
    int lastStable = 0;
    int debounceDelay;
    Debounce(int d) : debounceDelay(d) {}
    inline void IRAM_ATTR reset(int val) { lastStable = val; }
    inline bool IRAM_ATTR debounce(int val, int elapsed = 1) {
        if (val == last) {
            stableTime += elapsed;
        } else {
            last = val;
            stableTime = 0;
        }
        if (stableTime >= debounceDelay && val != lastStable) {
            lastStable = val;
            return true;
        }
        return false;
    }
};

class SysMonitor {
public:
    SysMonitorMenu *rootMenu;
    SysMonitorMenu *menu;
    // Integer milliseconds (the hardware divider; no float library calls).
    int32_t activeTimeoutMs = 0;
    bool exitRequested = false;
    uint8_t screenMem[24 * 40];
    Debounce consoleDebounce = Debounce(240 * 1000 * 30);
    Debounce keyboardDebounce = Debounce(240 * 1000 * 30);
    PbiIocb *pbiRequest = nullptr;
    uint32_t lastTsc = 0;
    uint32_t tickRemainder = 0;

    SysMonitor();   // boot (cold): builds the menus
    // In internal RAM, never PSRAM: the hot path reads it.
    static void *operator new(size_t size);
    static void operator delete(void *p);

    // One monitor pass for a PBI command 11: hot-safe.
    void pbi(PbiIocb *p);
    // Work the hot path queued (a cartridge change): run it with the bus
    // cold, e.g. right after pbi() from handlePbiRequest2().
    bool coldWorkPending() const;
    void runColdWork();
    void requestCart(const char *name);   // queues a cartridge change

    void saveScreen();
    void clearScreen();
    void restoreScreen();
    void drawScreen();
    void writeAt(int x, int y, const char *s, bool inv);
    void onKey(int key);
    void onConsoleKey(uint8_t key);

private:
    char pendingCart[40] = "";   // a cartridge picked, to open cold
};

extern SysMonitor *sysMonitor;

// sysMonitorCold.cpp: the parts that use std:: and I/O; cold bus only.
void sysMonitorListCarts(void (*add)(const char *name, void *ctx), void *ctx);
void sysMonitorSetCart(const char *name);
