#pragma once

// Shared by both apps: main and the Sidecar (sidecar/main/CMakeLists.txt)
// build sysConfig.cpp and configText.cpp, and both load the same BOOT_CONFIG
// profile.
//
// The configuration is two halves:
//   AtariConfig: what the Atari sees (cartridge, disks, memory layout, boot
//     keys, interrupts). The user's half: configText.cpp writes and reads it
//     as a text file ("key = value" lines), one file per configuration.
//   EspConfig: the boards' own environment (WiFi, servers, consoles, test
//     timeouts). Never in a configuration file; set by the build and the
//     built-in profiles only.
// SysConfig inherits both, so code reads config.<field> whichever half it's in.
#include <stdint.h>
#include <string>
#include <vector>
#include "esp_attr.h"
#include "extMemConfig.h"

using std::string;

// A string the bus-hot code may read (the boot keys, read from core0Loop()):
// its bytes are in internal RAM, never PSRAM (main's malloc prefers PSRAM:
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0) or flash (a literal). Set it cold;
// c_str() is inline, so a hot reader calls nothing in flash.
extern const char hotStrEmpty[1];
class HotStr {
    char *p_ = nullptr;
    void set(const char *s);
public:
    HotStr() {}
    HotStr(const char *s) { set(s); }
    HotStr(const HotStr &o) { set(o.p_); }
    HotStr &operator=(const HotStr &o) { if (this != &o) set(o.p_); return *this; }
    HotStr &operator=(const char *s) { set(s); return *this; }
    ~HotStr();
    const char *c_str() const { return p_ ? p_ : hotStrEmpty; }
    operator const char *() const { return c_str(); }
};

struct AtariConfig {
    string cartImage;
    string diskSpec[8];
    string page6BinFile;
    bool fakeCio = false;
    HotStr bootKeyboardInput;
    bool disablePbi = false;
    int interruptStartDelay = 10;
    volatile int interruptTicks = 240 * 1001 * 1001 / 5;  // 5Hz; <= 0 none
    int sysMonitorSec = 0;
    bool haltAvailable = false;
    int extMemSramBanks = 0;
    int baseMemSz = 2 * 1024;
    bool enableBasic = false;
    int diskIdleSwapDelay = -1;
    int diskIdleSwapRepeat = 0;
    HotStr diskIdleSwapKeys;   // the utility loop reads it with the bus hot
    ExtMemLayout::ExtMemConfig extMemConf = ExtMemLayout::NONE;
    // AUTO memory (docs/memory-autodetect.md): boot with 64K of base RAM
    // and no board banks; after the PBI-init memory probe, autoMemConfig()
    // (pbi.cpp) shrinks the base RAM to autoMinBaseRam when native RAM
    // reaches 48K and builds extended banks (ExtBankPool AUTO), leaving at
    // least autoMinFreeSram (EspConfig) bytes of internal RAM free.
    bool autoMem = false;
    int autoMinBaseRam = 2 * 1024;

    // configText.cpp, which documents the text form: KEY=VALUE lines, '#'
    // comments.
    // toText(): every field (with a comment line each when `comments`), or
    // with `base`, "base = <baseName>" and only the fields that differ from it.
    string toText(bool comments = true, const AtariConfig *base = nullptr, const char *baseName = nullptr) const;
    // Parse text over this object's current values. A "base = <name>" line
    // (the first setting) first replaces them with resolveBase(name)'s, or
    // the built-in profile's (SysConfig::loadBuiltin()) without one. Errors
    // ("line <n>: ...") go to errors; the other lines still apply. True if none.
    struct Error { int line; string msg; };
    typedef bool (*BaseResolver)(const string &name, AtariConfig &out);
    bool fromText(const char *text, std::vector<Error> *errors = nullptr, BaseResolver resolveBase = nullptr);
    // Files (VFS paths: the caller resolves them). saveFile() writes
    // <path>.part, then renames it over <path>. loadFile(): fromText() of the
    // file; false (with an Error, line 0) if it can't be read.
    bool saveFile(const char *path, bool comments = true, const AtariConfig *base = nullptr, const char *baseName = nullptr) const;
    bool loadFile(const char *path, std::vector<Error> *errors = nullptr, BaseResolver resolveBase = nullptr);
    bool operator==(const AtariConfig &o) const;
    bool operator!=(const AtariConfig &o) const { return !(*this == o); }
};

struct EspConfig {
    static constexpr uint16_t WD_DISABLE = 0xffff;
    int runSec = -1;
    int startupDelayMs = 0;
    int ioTimeoutSec = 120, wdTimeoutSec = 120;
    uint16_t telnetServerPort = 23;
    uint16_t httpServerPort = 80;   // the Sidecar's web server; 0: none
    bool telnetServerReadonly = false;
    uint16_t wdMemLoc = WD_DISABLE;
    bool enableWifi = true;
    int wifiConnectDelay = 0;
    bool enableMdns = false;
    int serialStatusSec = 6;
    int serialScreenDumpSec = 90;
    bool logScreen = true;
    string wifiSSID = "ClemmyNet";
    string wifiPassword = "clementine is a cat";
    string wifiHostname = "busbang-$MAC";
    int autoMinFreeSram = 16 * 1024;   // AUTO memory's internal RAM floor (AtariConfig::autoMem)
};

class SysConfig : public AtariConfig, public EspConfig {
public:
    bool loaded = false;   // load() has run (csim: the Sidecar's coroutine waits for main's)
    SysConfig();
    void save();   // main only (main.cpp): the cart choice, to main's spiffs
    void load(string configName = "");
    // The built-in profile `name` over the defaults (both halves); false
    // (and the defaults) if there's no such profile.
    bool loadBuiltin(const string &name);
    static const char *const builtinNames[];   // nullptr-terminated
};

extern SysConfig config;
