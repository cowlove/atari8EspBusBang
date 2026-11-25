// AtariConfig's text form (sysConfig.h): a boot configuration as a file a
// person reads and edits. Built by both apps, like sysConfig.cpp. Cold code
// only (heap, stdio): never called with the bus hot.
//
//   # SDX on my 130XE
//   BASE=SDX_AUTO
//   D1=sidecar!/sd/sdx451.atr
//   CART=sidecar!/sd/SDX451_atarimax128.car.gz
//   BASE_RAM=64K
//   BOOT_KEYS="-2:X\233"
//
// - One KEY=VALUE per line; keys in any case; spaces around '=' ignored.
// - '#' starts a comment: a whole line, or after a space at the end of one.
// - Strings: as written, or in double quotes, where \" \\ and octal \NNN
//   escapes work (\233 is the ATASCII RETURN) and the value may hold spaces
//   at its ends or a " #". Bare values take no escapes (paths keep their \).
// - Booleans: YES/NO (TRUE/FALSE, ON/OFF, 1/0). Numbers: decimal or 0x hex,
//   a K suffix for 1024s.
// - BASE=<name>, the first setting: start from that configuration (a
//   built-in profile unless the caller resolves names) instead of the
//   object's current values; the lines after it change it.
// - An unknown key or a bad value is an error with its line number; the
//   other lines still apply.
#ifdef CSIM
#include "csim.h"
#endif
#include "sysConfig.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

enum class Kind : uint8_t { Str, Hot, Bool, Int, Size, ExtMem, Interrupt };

struct Field {
    const char *key;
    Kind kind;
    void *(*at)(AtariConfig &);
    const char *help;
    bool emit;   // written by toText() (INTERRUPT_TICKS: only when INTERRUPT_HZ can't say it)
};

#define CFG_FIELD(key, kind, member, help) \
    { key, Kind::kind, [](AtariConfig &c) -> void * { return (void *)&c.member; }, help, true }
#define CFG_DISK(n) CFG_FIELD("D" #n, Str, diskSpec[n - 1], nullptr)

const Field fields[] = {
    CFG_FIELD("CART", Str, cartImage, "cartridge image (empty: none)"),
    CFG_FIELD("D1", Str, diskSpec[0], "drives D1-D8: an image path or mount spec (empty: none)"),
    CFG_DISK(2), CFG_DISK(3), CFG_DISK(4),
    CFG_DISK(5), CFG_DISK(6), CFG_DISK(7), CFG_DISK(8),
    CFG_FIELD("BOOT_KEYS", Hot, bootKeyboardInput, "typed 15 s after the first I/O (\\233: RETURN)"),
    CFG_FIELD("BASIC", Bool, enableBasic, "leave the built-in BASIC enabled"),
    CFG_FIELD("BASE_RAM", Size, baseMemSz, "main's base RAM mapped from $0000"),
    CFG_FIELD("EXT_MEM", ExtMem, extMemConf, "extended memory: NONE, RAMBO256, COMPY192, XE, AUTO"),
    CFG_FIELD("EXT_MEM_SRAM_BANKS", Int, extMemSramBanks, "extended banks kept in internal RAM (the rest in PSRAM)"),
    CFG_FIELD("HALT_LINE", Bool, haltAvailable, "the machine has the halt (RDY) wire: needed for PSRAM banks"),
    CFG_FIELD("AUTO_MEM", Bool, autoMem, "size base RAM and banks from the boot-time memory probe"),
    CFG_FIELD("AUTO_MIN_BASE_RAM", Size, autoMinBaseRam, "AUTO_MEM's base RAM when native RAM reaches 48K"),
    CFG_FIELD("INTERRUPT_HZ", Interrupt, interruptTicks, "PBI interrupts per second (0: none)"),
    { "INTERRUPT_TICKS", Kind::Int, [](AtariConfig &c) -> void * { return (void *)&c.interruptTicks; },
      "the interrupt period in 240 MHz ticks (<= 0: none)", false },
    CFG_FIELD("INTERRUPT_START_SEC", Int, interruptStartDelay, "seconds before interrupts start without I/O"),
    CFG_FIELD("SYSMON_SEC", Int, sysMonitorSec, "open the system monitor every N seconds (0: never)"),
    CFG_FIELD("DISK_SWAP_IDLE_SEC", Int, diskIdleSwapDelay, "rotate the drives after N idle seconds (-1: never)"),
    CFG_FIELD("DISK_SWAP_REPEAT", Int, diskIdleSwapRepeat, "rotations after the first"),
    CFG_FIELD("DISK_SWAP_KEYS", Hot, diskIdleSwapKeys, "typed at each rotation"),
    CFG_FIELD("PAGE6_FILE", Str, page6BinFile, "a binary loaded into page 6"),
    CFG_FIELD("FAKE_CIO", Bool, fakeCio, "test: the fake CIO handler"),
    CFG_FIELD("DISABLE_PBI", Bool, disablePbi, "test: no PBI device"),
};

const struct { const char *name; ExtMemLayout::ExtMemConfig value; } extMemNames[] = {
    { "NONE", ExtMemLayout::NONE }, { "RAMBO256", ExtMemLayout::RAMBO256 },
    { "COMPY192", ExtMemLayout::NATIVE_XE_COMPY192 }, { "XE", ExtMemLayout::XE },
    { "AUTO", ExtMemLayout::AUTO },
};

constexpr int64_t kTicksPerHz = 240LL * 1001 * 1001;   // the profiles' "240 * 1001 * 1001 / hz"

const Field *findField(const string &key) {
    for (const Field &f : fields)
        if (strcasecmp(f.key, key.c_str()) == 0) return &f;
    return nullptr;
}

string quoteIfNeeded(const char *s) {
    size_t n = strlen(s);
    bool plain = n == 0 || (s[0] != ' ' && s[n - 1] != ' ' && s[0] != '"' && !strstr(s, " #"));
    for (size_t i = 0; plain && i < n; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) plain = false;
    if (plain) return s;
    string out = "\"";
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = s[i];
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if (ch < 0x20 || ch > 0x7e) { char oct[5]; snprintf(oct, sizeof(oct), "\\%03o", ch); out += oct; }
        else out += ch;
    }
    return out + "\"";
}

string formatInt(int v, bool size) {
    char buf[24];
    if (size && v != 0 && v % 1024 == 0) snprintf(buf, sizeof(buf), "%dK", v / 1024);
    else snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}

// The key and value toText() writes for f.
void formatField(const Field &f, const AtariConfig &cc, string &key, string &value) {
    AtariConfig &c = const_cast<AtariConfig &>(cc);
    void *p = f.at(c);
    key = f.key;
    switch (f.kind) {
    case Kind::Str: value = quoteIfNeeded(((string *)p)->c_str()); break;
    case Kind::Hot: value = quoteIfNeeded(((HotStr *)p)->c_str()); break;
    case Kind::Bool: value = *(bool *)p ? "YES" : "NO"; break;
    case Kind::Int: value = formatInt(*(int *)p, false); break;
    case Kind::Size: value = formatInt(*(int *)p, true); break;
    case Kind::ExtMem:
        value = formatInt(*(int *)p, false);
        for (auto &e : extMemNames)
            if (e.value == *(ExtMemLayout::ExtMemConfig *)p) value = e.name;
        break;
    case Kind::Interrupt: {
        int ticks = *(volatile int *)p;
        int64_t hz = ticks > 0 ? (kTicksPerHz + ticks / 2) / ticks : 0;
        if (ticks == 0) value = "0";
        else if (hz > 0 && kTicksPerHz / hz == ticks) value = formatInt((int)hz, false);
        else { key = "INTERRUPT_TICKS"; value = formatInt(ticks, false); }
        break;
    }
    }
}

bool fieldEqual(const Field &f, const AtariConfig &a, const AtariConfig &b) {
    void *p = f.at(const_cast<AtariConfig &>(a)), *q = f.at(const_cast<AtariConfig &>(b));
    switch (f.kind) {
    case Kind::Str: return *(string *)p == *(string *)q;
    case Kind::Hot: return strcmp(((HotStr *)p)->c_str(), ((HotStr *)q)->c_str()) == 0;
    case Kind::Bool: return *(bool *)p == *(bool *)q;
    case Kind::ExtMem: return *(ExtMemLayout::ExtMemConfig *)p == *(ExtMemLayout::ExtMemConfig *)q;
    default: return *(volatile int *)p == *(volatile int *)q;
    }
}

bool parseInt(const string &s, int &out) {
    if (s.empty()) return false;
    char *end;
    long v = strtol(s.c_str(), &end, 0);
    if (*end == 'k' || *end == 'K') { v *= 1024; end++; }
    if (*end) return false;
    out = (int)v;
    return true;
}

bool parseBool(const string &s, bool &out) {
    static const char *const yes[] = { "YES", "TRUE", "ON", "1" }, *const no[] = { "NO", "FALSE", "OFF", "0" };
    for (const char *w : yes) if (strcasecmp(s.c_str(), w) == 0) { out = true; return true; }
    for (const char *w : no) if (strcasecmp(s.c_str(), w) == 0) { out = false; return true; }
    return false;
}

// Apply one value; an error message, or "" if it took.
string applyField(const Field &f, const string &v, AtariConfig &c) {
    void *p = f.at(c);
    int n;
    switch (f.kind) {
    case Kind::Str: *(string *)p = v; return "";
    case Kind::Hot: *(HotStr *)p = v.c_str(); return "";
    case Kind::Bool: return parseBool(v, *(bool *)p) ? "" : "not YES or NO: " + v;
    case Kind::Int:
    case Kind::Size:
        if (!parseInt(v, n)) return "not a number: " + v;
        *(volatile int *)p = n;
        return "";
    case Kind::ExtMem:
        for (auto &e : extMemNames)
            if (strcasecmp(v.c_str(), e.name) == 0) { *(ExtMemLayout::ExtMemConfig *)p = e.value; return ""; }
        return "not an extended memory layout: " + v;
    case Kind::Interrupt: {
        char *end;
        double hz = strtod(v.c_str(), &end);
        if (v.empty() || *end || hz < 0) return "not a rate: " + v;
        *(volatile int *)p = hz == 0 ? 0 : (int)(kTicksPerHz / hz);
        return "";
    }
    }
    return "?";
}

string trim(const string &s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == string::npos ? "" : s.substr(a, b - a + 1);
}

// A value as written: quoted (escapes, then nothing but a comment) or bare
// (up to a " #" comment). An error message, or "".
string parseValue(const string &raw, string &out) {
    out.clear();
    if (raw.empty() || raw[0] != '"') {
        size_t hash = raw.find(" #");
        if (raw.size() > 0 && raw[0] == '#') hash = 0;
        out = trim(hash == string::npos ? raw : raw.substr(0, hash));
        return "";
    }
    size_t i = 1;
    for (; i < raw.size() && raw[i] != '"'; i++) {
        char ch = raw[i];
        if (ch != '\\') { out += ch; continue; }
        if (++i >= raw.size()) break;
        ch = raw[i];
        if (ch >= '0' && ch <= '7') {
            int v = 0, digits = 0;
            while (digits < 3 && i < raw.size() && raw[i] >= '0' && raw[i] <= '7') { v = v * 8 + (raw[i++] - '0'); digits++; }
            i--;
            out += (char)v;
        } else if (ch == 'n') out += '\n';
        else out += ch;
    }
    if (i >= raw.size()) return "no closing quote";
    string rest = trim(raw.substr(i + 1));
    if (!rest.empty() && rest[0] != '#') return "text after the closing quote: " + rest;
    return "";
}

bool builtinBase(const string &name, AtariConfig &out) {
    SysConfig profile;
    if (!profile.loadBuiltin(name)) return false;
    out = profile;
    return true;
}

}  // namespace

bool AtariConfig::operator==(const AtariConfig &o) const {
    for (const Field &f : fields)
        if (!fieldEqual(f, *this, o)) return false;
    return true;
}

string AtariConfig::toText(bool comments, const AtariConfig *base, const char *baseName) const {
    string out;
    if (comments)
        out += "# A boot configuration (main/configText.cpp): KEY=VALUE, '#' comments;\n"
               "# strings in \"quotes\" take \\\" \\\\ and octal \\NNN escapes.\n";
    if (base) {
        out += "BASE=";
        out += baseName ? baseName : "";
        out += "\n";
    }
    for (const Field &f : fields) {
        if (!f.emit || (base && fieldEqual(f, *this, *base))) continue;
        string key, value;
        formatField(f, *this, key, value);
        if (comments && f.help) { out += "# "; out += f.help; out += "\n"; }
        out += key + "=" + value + "\n";
    }
    return out;
}

bool AtariConfig::fromText(const char *text, std::vector<Error> *errors, BaseResolver resolveBase) {
    bool ok = true, anySetting = false;
    auto fail = [&](int line, const string &msg) {
        ok = false;
        if (errors) errors->push_back({ line, msg });
    };
    int lineNo = 0;
    for (const char *p = text; p && *p;) {
        const char *nl = strchr(p, '\n');
        string line = trim(nl ? string(p, nl - p) : string(p));
        p = nl ? nl + 1 : nullptr;
        lineNo++;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == string::npos) { fail(lineNo, "no '=': " + line); continue; }
        string key = trim(line.substr(0, eq)), value;
        string err = parseValue(trim(line.substr(eq + 1)), value);
        if (!err.empty()) { fail(lineNo, key + ": " + err); continue; }
        if (strcasecmp(key.c_str(), "BASE") == 0) {
            if (anySetting) { fail(lineNo, "BASE must be the first setting"); continue; }
            AtariConfig base;
            if (!(resolveBase ? resolveBase : builtinBase)(value, base)) { fail(lineNo, "no configuration " + value); continue; }
            *this = base;
            anySetting = true;
            continue;
        }
        anySetting = true;
        const Field *f = findField(key);
        if (!f) { fail(lineNo, "unknown key " + key); continue; }
        err = applyField(*f, value, *this);
        if (!err.empty()) fail(lineNo, key + ": " + err);
    }
    return ok;
}

bool AtariConfig::saveFile(const char *path, bool comments, const AtariConfig *base, const char *baseName) const {
    const string text = toText(comments, base, baseName);
    const string part = string(path) + ".part";
    FILE *f = fopen(part.c_str(), "w");
    if (!f) return false;
    const bool wrote = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0 || !wrote) { unlink(part.c_str()); return false; }
    unlink(path);   // spiffs' rename() won't replace a file
    return rename(part.c_str(), path) == 0;
}

bool AtariConfig::loadFile(const char *path, std::vector<Error> *errors, BaseResolver resolveBase) {
    constexpr size_t kMaxBytes = 16 * 1024;   // a configuration is a page or two of text
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errors) errors->push_back({ 0, string("can't open ") + path });
        return false;
    }
    string text;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < kMaxBytes) text.append(buf, n);
    fclose(f);
    if (text.size() >= kMaxBytes) {
        if (errors) errors->push_back({ 0, string(path) + ": over 16 KB" });
        return false;
    }
    return fromText(text.c_str(), errors, resolveBase);
}
