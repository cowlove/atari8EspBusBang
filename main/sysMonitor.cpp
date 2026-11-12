// The system monitor; see sysMonitor.h.
//
// Everything here can run hot (SysMonitor::pbi() and what it calls), so the
// whole object's code and constants are placed in internal RAM (main/link.lf,
// "sysMonitor (noflash)"), the menu items live in internal RAM (operator new
// below) and nothing here uses std:: containers or strings: ETL's
// fixed-capacity ones instead (components/etl). The classes are private to
// this file so their vtables and template code are emitted only here, never
// as a flash copy in another object. I/O and std:: live in sysMonitorCold.cpp.
#ifdef CSIM
#include "csim.h"
#else
#include "sdkconfig.h"
#endif
#if CONFIG_A8BB_SYSMONITOR   // the whole file

#include "sysMonitor.h"
#include "sysConfig.h"
#include "asmDefs.h"
#include "mmu.h"

#include <initializer_list>
#include <stdlib.h>
#include <string.h>

#include "etl/string.h"
#include "etl/to_string.h"
#include "etl/vector.h"

#ifndef CSIM
#include "esp_heap_caps.h"
#endif

extern uint8_t pbiROM[];
extern volatile int sysMonitorTime;

// Code that never runs hot (building the menus at boot, the cold work) stays
// in flash: main/link.lf maps this section back out of the object's
// internal-RAM placement.
#define SYSMON_COLD __attribute__((section(".text.sysmon_cold")))

namespace {

// Screen rows 11..23 hold a menu's options, so a menu shows at most 13.
constexpr size_t kMaxOptions = 13;
using MonLabel = etl::string<32>;
using MonValue = etl::string<32>;
using MonTitle = etl::string<38>;
using MonText = etl::string<40>;   // one screen line

typedef void (*StrFn)(const char *);
typedef void (*BoolFn)(bool);

SYSMON_COLD void *internalAlloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == nullptr)
        abort();   // at boot, building the menus
    return p;
}

// "label : value", truncated to the line.
void labelValue(MonText &out, const MonLabel &label, const MonValue &value)
{
    out.assign(label.begin(), label.end());
    out.append(" : ");
    out.append(value.begin(), value.end());
}

// Decimal, as sscanf("%d") read the old text fields; false if no digits.
bool parseInt(const char *s, int &out)
{
    while (*s == ' ')
        s++;
    bool neg = *s == '-';
    if (neg || *s == '+')
        s++;
    if (*s < '0' || *s > '9')
        return false;
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    out = neg ? -v : v;
    return true;
}

void appendHex2(MonText &s, unsigned v)
{
    static const char hex[] = "0123456789abcdef";
    s.push_back(hex[(v >> 4) & 15]);
    s.push_back(hex[v & 15]);
}

}   // namespace

class SysMonitorMenuItem {
public:
    MonLabel label;
    MonValue value;
    SysMonitorMenu *parent = nullptr;
    bool selectable = true;
    bool pickable = false;
    bool showCursor = false;
    bool group = false;
    SysMonitorMenuItem(const char *t) : label(t), value(t) {}
    SysMonitorMenuItem() {}
    virtual ~SysMonitorMenuItem();
    // The item's line on the screen.
    virtual void render(MonText &out) const { out.assign(label.begin(), label.end()); }
    virtual void onSelect(SysMonitor *) {}
    virtual void onKey(SysMonitor *, int key);
    virtual void setChecked(bool) {}   // radio-button groups
    static void *operator new(size_t size) { return internalAlloc(size); }
    static void operator delete(void *p) { free(p); }
};

class SysMonitorMenu : virtual public SysMonitorMenuItem {
public:
    MonTitle title;
    etl::vector<SysMonitorMenuItem *, kMaxOptions> options;
    int selected = 0;
    SysMonitorMenu() {}
    SysMonitorMenu(const char *t, const char *l, std::initializer_list<SysMonitorMenuItem *> v)
        : SysMonitorMenuItem(l), title(t) {
        for (auto o : v)
            add(o);
    }
    ~SysMonitorMenu() {
        for (auto o : options)
            delete o;
    }
    // More than kMaxOptions: the rest are dropped (they'd be off the screen).
    void add(SysMonitorMenuItem *i) {
        if (options.full()) {
            delete i;
            return;
        }
        options.push_back(i);
        i->parent = this;
    }
    void onSelect(SysMonitor *m) override;
};

class SysMonitorMenuPlaceholder : public SysMonitorMenuItem {
public:
    SysMonitorMenuPlaceholder(const char *t) : SysMonitorMenuItem(t) { selectable = false; }
};

class SysMonitorPickOneChoice : virtual public SysMonitorMenuItem {
public:
    SysMonitorPickOneChoice(const char *t) : SysMonitorMenuItem(t) { pickable = true; }
    void onSelect(SysMonitor *) override;
};

class SysMonitorPickOne : public SysMonitorMenu {
public:
    StrFn onChange;
    SysMonitorPickOne(const char *t, const char *l, const char *def,
                      std::initializer_list<SysMonitorMenuItem *> v, StrFn f = nullptr)
        : SysMonitorMenuItem(l), SysMonitorMenu(t, l, v), onChange(f) {
        label = l;
        value = def;
        pickable = true;
    }
    void setValue(const MonValue &s) {
        value = s;
        if (onChange != nullptr)
            onChange(value.c_str());
    }
    void render(MonText &out) const override { labelValue(out, label, value); }
};

class SysMonitorMenuItemBoolean : public SysMonitorMenuItem {
public:
    bool on = false;
    BoolFn onChange;
    SysMonitorMenuItemBoolean(const char *t, BoolFn f = nullptr) : SysMonitorMenuItem(t), onChange(f) {}
    void render(MonText &out) const override {
        out.assign(on ? "[X] " : "[ ] ");
        out.append(label.begin(), label.end());
    }
    void onSelect(SysMonitor *) override {
        on = !on;
        if (onChange != nullptr)
            onChange(on);
    }
    void setChecked(bool v) override { on = v; }
};

class SysMonitorMenuItemRadioButton : public SysMonitorMenuItemBoolean {
public:
    SysMonitorMenuItemRadioButton(const char *t, BoolFn f = nullptr) : SysMonitorMenuItemBoolean(t, f) {
        group = true;
    }
    // Unchecks the rest of its group: the adjacent radio buttons.
    void onSelect(SysMonitor *) override {
        for (int i = parent->selected - 1; i >= 0 && parent->options[i]->group; i--)
            parent->options[i]->setChecked(false);
        for (int i = parent->selected + 1; i < (int)parent->options.size() && parent->options[i]->group; i++)
            parent->options[i]->setChecked(false);
        on = true;
        if (onChange != nullptr)
            onChange(on);
    }
};

class SysMonitorMenuItemText : virtual public SysMonitorMenuItem {
public:
    StrFn onChange;
    SysMonitorMenuItemText(const char *t, const char *def, StrFn f = nullptr)
        : SysMonitorMenuItem(t), onChange(f) {
        label = t;
        value = def;
        showCursor = true;
    }
    void render(MonText &out) const override { labelValue(out, label, value); }
    void onSelect(SysMonitor *) override {}
    void onKey(SysMonitor *m, int key) override;
    void setValue(const MonValue &s) {
        if (onChange != nullptr)
            onChange(s.c_str());
    }
};

class PickOneChoiceEditable : public SysMonitorPickOneChoice, SysMonitorMenuItemText {
public:
    PickOneChoiceEditable(const char *t, const char *def = "")
        : SysMonitorMenuItem(t), SysMonitorPickOneChoice(t), SysMonitorMenuItemText(t, def) {
        pickable = true;
        showCursor = true;
    }
    void render(MonText &out) const override { SysMonitorMenuItemText::render(out); }
    void onSelect(SysMonitor *) override {}
    void onKey(SysMonitor *, int) override;
};

class PickOneChoiceSubmenu : public SysMonitorPickOneChoice, public SysMonitorPickOne {
public:
    PickOneChoiceSubmenu(const char *t, const char *l, const char *def,
                         std::initializer_list<SysMonitorMenuItem *> v, StrFn f = nullptr)
        : SysMonitorMenuItem(l), SysMonitorPickOneChoice(l), SysMonitorPickOne(t, l, def, v, f) {
        pickable = true;
    }
    void render(MonText &out) const override { SysMonitorPickOne::render(out); }
    void onSelect(SysMonitor *m) override { SysMonitorMenu::onSelect(m); }
};

class MenuBack : public SysMonitorMenuItem {
public:
    MenuBack() : SysMonitorMenuItem("EXIT") {}
    void onSelect(SysMonitor *) override;
};

// The key function: the vtables are emitted here, in this object.
SysMonitorMenuItem::~SysMonitorMenuItem() {}

void SysMonitorMenuItem::onKey(SysMonitor *m, int key) {
    if (parent != nullptr)
        parent->onKey(m, key);
}

void SysMonitorMenu::onSelect(SysMonitor *m) {
    m->menu = this;
    m->clearScreen();
}

void MenuBack::onSelect(SysMonitor *m) {
    m->menu = m->menu->parent;
    m->clearScreen();
}

// Sets the choice's value on its PickOne, and on each pickable menu above it
// (a choice inside a PickOneChoiceSubmenu), leaving a menu level each time.
void SysMonitorPickOneChoice::onSelect(SysMonitor *m) {
    SysMonitorMenu *p = parent;
    while (p != nullptr && p->pickable) {
        static_cast<SysMonitorPickOne *>(p)->setValue(value);
        m->menu = m->menu->parent;
        p = p->parent;
    }
    m->clearScreen();
}

void SysMonitorMenuItemText::onKey(SysMonitor *m, int key) {
    if (key == 127) {
        if (!value.empty())
            value.pop_back();
    } else if (key == 10 || key == 233) {
        setValue(value);
    } else {
        value.push_back((char)key);
    }
}

void PickOneChoiceEditable::onKey(SysMonitor *m, int key) {
    if (key == 127) {
        if (!value.empty())
            value.pop_back();
    } else if (key == 10 || key == 233) {
        setValue(value);
        SysMonitorPickOneChoice::onSelect(m);
    } else {
        value.push_back((char)key);
    }
}

namespace {

SYSMON_COLD SysMonitorMenuItem *diskPicker(int n) {
    MonTitle title("CHOOSE DISK "), flash("CHOOSE D"), smb("CHOOSE D"), smbDir("CHOOSE D");
    MonLabel label("DISK ");
    etl::to_string(n, title, true);
    etl::to_string(n, label, true);
    label.append("   ");
    etl::to_string(n, flash, true);
    flash.append(": FLASH IMAGE");
    etl::to_string(n, smb, true);
    smb.append(": SMB IMAGE");
    etl::to_string(n, smbDir, true);
    smbDir.append(": SMB DIR TO MOUNT");
    return new SysMonitorPickOne(
        title.c_str(), label.c_str(), "<NONE>",
        {
            new SysMonitorPickOneChoice("<NONE>"),
            new PickOneChoiceSubmenu(flash.c_str(), "FLASH DISK IMAGE", "",
                {
                    new SysMonitorPickOneChoice("FLASH1.ATR"),
                    new SysMonitorPickOneChoice("FLASH2.ATR"),
                }),
            new PickOneChoiceSubmenu(smb.c_str(), "SMB DISK IMAGE", "",
                {
                    new SysMonitorMenuItemText("SEARCH SMB PATH", "//host/share/dir"),
                    new SysMonitorPickOneChoice("SMB1.ATR"),
                    new SysMonitorPickOneChoice("SMB2.ATR"),
                }),
            new PickOneChoiceSubmenu(smbDir.c_str(), "SMB MOUNT", "",
                {
                    new SysMonitorMenuItemText("SMB PATH", "//host/share/dir"),
                    new SysMonitorMenuItemBoolean("SPARTADOS 4.5 IMAGE"),
                    new SysMonitorMenuPlaceholder(""),
                    new SysMonitorMenuPlaceholder("PICK FROM AVAILABLE DIRS:"),
                    new SysMonitorPickOneChoice("DIR1"),
                    new SysMonitorPickOneChoice("DIR2"),
                }),
            new MenuBack(),
        });
}

SYSMON_COLD void addCartChoice(const char *name, void *menu) {
    static_cast<SysMonitorMenu *>(menu)->add(new SysMonitorPickOneChoice(name));
}

SYSMON_COLD SysMonitorMenuItem *cartridgePicker() {
    auto *flash = new PickOneChoiceSubmenu("CHOOSE CART FLASH IMAGE", "FLASH IMAGE", "", {});
    sysMonitorListCarts(addCartChoice, static_cast<SysMonitorMenu *>(flash));
    return new SysMonitorPickOne(
        "SELECTED CARTRIDGE IMAGE", "CARTRIDGE", "<NONE>",
        {
            new SysMonitorPickOneChoice("<NO CARTRIDGE>"),
            flash,
            new PickOneChoiceSubmenu("CHOOSE CART SMB IMAGE", "SMB IMAGE", "",
                {
                    new SysMonitorMenuItemText("SEARCH SMB PATH", "//host/share/dir"),
                    new SysMonitorPickOneChoice("SMB1.CAR"),
                    new SysMonitorPickOneChoice("SMB2.CAR"),
                }),
            new SysMonitorMenuPlaceholder(""),
            new MenuBack(),
        },
        // Opening a cartridge is I/O: runColdWork() does it.
        [](const char *s) { sysMonitor->requestCart(s); });
}

}   // namespace

SYSMON_COLD void *SysMonitor::operator new(size_t size) { return internalAlloc(size); }
void SysMonitor::operator delete(void *p) { free(p); }

SYSMON_COLD SysMonitor::SysMonitor() {
    rootMenu = new SysMonitorMenu("MAIN MENU", "", {
        cartridgePicker(),
        diskPicker(1),
        diskPicker(2),
        new SysMonitorMenu("", "MORE DISKS", {
            diskPicker(3),
            diskPicker(4),
            diskPicker(5),
            diskPicker(6),
            diskPicker(7),
            diskPicker(8),
            new MenuBack(),
        }),
        new SysMonitorMenu("AUTO DISK SWAP", "AUTO DISK SWAP", {
            new SysMonitorMenuItemBoolean("ENABLE AUTO DISK SWAP"),
            new SysMonitorMenuItemText("ROTATE DISKS INACTIVITY SEC", "3"),
            new SysMonitorMenuItemText("THEN PRESS KEYS", "[RETURN]"),
            new SysMonitorMenuItemText("REPEAT MULTIPLE TIMES", "0"),
            new SysMonitorMenuPlaceholder(""),
            new MenuBack(),
        }),
        new SysMonitorMenuPlaceholder(""),
        new SysMonitorMenuItemBoolean("INVERT OPTION KEY ON BOOT"),
        new SysMonitorMenuItemText("START MONITOR EVERY N SECONDS", "10", [](const char *v) {
            int sec;
            if (parseInt(v, sec))
                sysMonitorTime = sec;
        }),
        new SysMonitorMenuItemText("PBI INTERRUPTS PER SECOND", "10", [](const char *v) {
            int intPerSec;
            if (parseInt(v, intPerSec) && intPerSec > 0)
                config.interruptTicks = 240 * 1001 * 1001 / intPerSec;
        }),
        new SysMonitorMenuPlaceholder(""),
        new SysMonitorMenuItemRadioButton("OPT 3"),
        new SysMonitorMenuItemRadioButton("OPT 4"),
        new SysMonitorMenuItemRadioButton("OPT 5"),
    });
    menu = rootMenu;
}

void SysMonitor::requestCart(const char *name) {
    size_t i = 0;
    for (; name[i] != 0 && i < sizeof(pendingCart) - 1; i++)
        pendingCart[i] = name[i];
    pendingCart[i] = 0;
}

bool SysMonitor::coldWorkPending() const { return pendingCart[0] != 0; }

SYSMON_COLD void SysMonitor::runColdWork() {
    if (pendingCart[0] != 0) {
        sysMonitorSetCart(pendingCart);
        pendingCart[0] = 0;
    }
}

void SysMonitor::saveScreen() {
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    for (int i = 0; i < (int)sizeof(screenMem); i++)
        screenMem[i] = atariRam[savmsc + i];
}

void SysMonitor::clearScreen() {
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    for (int i = 0; i < (int)sizeof(screenMem); i++)
        atariRam[savmsc + i] = 0;
}

void SysMonitor::restoreScreen() {
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    for (int i = 0; i < (int)sizeof(screenMem); i++)
        atariRam[savmsc + i] = screenMem[i];
    atariRam[712] = 0;
    atariRam[710] = 148;
}

// ASCII at column x (x < 0: centered) of row y, clipped to the 40x24 screen.
void SysMonitor::writeAt(int x, int y, const char *s, bool inv) {
    if (y < 0 || y >= 24)
        return;
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    int len = strlen(s);
    if (x < 0)
        x = 20 - len / 2;
    for (int i = 0; i < len && x + i < 40; i++) {
        uint8_t c = s[i];
        if (c < 32)
            c += 64;
        else if (c < 96)
            c -= 32;
        atariRam[savmsc + y * 40 + x + i] = c + (inv ? 128 : 0);
    }
}

void SysMonitor::onKey(int key) {
    menu->options[menu->selected]->onKey(this, key);
}

void SysMonitor::onConsoleKey(uint8_t key) {
    const int last = (int)menu->options.size() - 1;
    if (key != 7)
        activeTimeoutMs = 600 * 1000;
    if (key == 6) {
        do {
            if (menu->selected < last)
                menu->selected++;
        } while (!menu->options[menu->selected]->selectable && menu->selected < last);
    }
    if (key == 3) {
        do {
            if (menu->selected > 0)
                menu->selected--;
        } while (!menu->options[menu->selected]->selectable && menu->selected > 0);
    }
    if (key == 5)
        menu->options[menu->selected]->onSelect(this);
    if (key == 0 || key == 1)
        exitRequested = true;
    if (key == 7 && exitRequested)
        activeTimeoutMs = 0;
}

void SysMonitor::drawScreen() {
    writeAt(-1, 2, " SYSTEM MONITOR ", true);
    writeAt(-1, 4, "Everything will be fine!", false);
    MonText line("Timeout: ");
    etl::to_string((int)((activeTimeoutMs + 500) / 1000), line, true);
    writeAt(-1, 5, line.c_str(), false);
    line.assign("KBCODE = ");
    appendHex2(line, pbiRequest->kbcode);
    line.append(" CONSOL = ");
    appendHex2(line, pbiRequest->consol);
    writeAt(-1, 7, line.c_str(), false);
    writeAt(-1, 9, menu->title.c_str(), false);
    const int xpos = 0, ypos = 11, cursorLen = 3;
    for (int i = 0; i < (int)menu->options.size(); i++) {
        writeAt(xpos, ypos + i, menu->selected == i ? "-> " : "   ", false);
        MonText text;
        menu->options[i]->render(text);
        writeAt(xpos + cursorLen, ypos + i, text.c_str(), menu->selected == i);
        bool cursorOn = menu->selected == i && menu->options[i]->showCursor &&
                        (XTHAL_GET_CCOUNT() & 0x4000000) == 0;
        writeAt(xpos + cursorLen + text.size(), ypos + i, " ", cursorOn);
        writeAt(xpos + cursorLen + text.size() + 1, ypos + i, "  ", false);
    }
    atariRam[712] = 255;
    atariRam[710] = 0;
}

void SysMonitor::pbi(PbiIocb *p) {
    pbiRequest = p;
    uint32_t tsc = XTHAL_GET_CCOUNT();
    if (activeTimeoutMs <= 0) {   // first reactivation, reinitialize
        lastTsc = tsc;
        activeTimeoutMs = 2000;
        if (pbiRequest->consol == 0 || pbiRequest->kbcode == 0xe5)
            activeTimeoutMs = 5000;
        exitRequested = false;
        menu->selected = 0;
        keyboardDebounce.reset(pbiRequest->kbcode);
        consoleDebounce.reset(pbiRequest->consol);
        saveScreen();
        clearScreen();
    }
    uint32_t elapsedTicks = tsc - lastTsc;
    lastTsc = tsc;
    if (activeTimeoutMs > 0) {
        // Whole milliseconds; the rest of a millisecond carries over.
        constexpr uint32_t kTicksPerMs = 240000;
        uint32_t ticks = elapsedTicks + tickRemainder;
        activeTimeoutMs -= ticks / kTicksPerMs;
        tickRemainder = ticks % kTicksPerMs;
        if (consoleDebounce.debounce(pbiRequest->consol, elapsedTicks))
            onConsoleKey(pbiRequest->consol);
        if (keyboardDebounce.debounce(pbiRequest->kbcode, elapsedTicks)) {
            // TODO: need to do keypressToAtascii[]
        }
        drawScreen();
        pbiRequest->result |= RES_FLAG_MONITOR;
    }
    if (activeTimeoutMs <= 0) {
        pbiRequest->result &= (~RES_FLAG_MONITOR);
        restoreScreen();
        activeTimeoutMs = 0;
        tickRemainder = 0;
    }
    pbiRequest->result |= RES_FLAG_COPYOUT;
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    int len = 40 * 24;
    for (int i = 0; i < len; i++)
        pbiROM[0x400 + i] = atariRam[savmsc + i];   // TODO get proper paddr for this
    pbiRequest->copybuf = savmsc;
    pbiRequest->copylen = len;
}

DRAM_ATTR SysMonitor *sysMonitor = NULL;

#endif   // CONFIG_A8BB_SYSMONITOR
