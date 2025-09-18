#pragma once 
#include <string>
#include <functional>
#include "pbi.h"
using std::string;
using std::function;
using std::min;
using std::max;
using std::vector;

#include "sfmt.h" 

class SysMonitor;
class SysMonitorMenu;

class SysMonitorMenuItem {
    public:
    string text, label, value;
    SysMonitorMenu *parent = NULL;
    SysMonitorMenuItem(const string &t) : text(t), label(t), value(t) {}
    SysMonitorMenuItem()  {}
    virtual ~SysMonitorMenuItem() {}
    virtual void onSelect(SysMonitor *) {};
    virtual void onKey(SysMonitor *, int key); 
    bool selectable = true;
    bool pickable = false;
};

class SysMonitorMenu : virtual public SysMonitorMenuItem {
public:
    string title;
    vector<SysMonitorMenuItem *> options;
    int selected = 0;
    SysMonitorMenu(const string &t, const string &l, const vector<SysMonitorMenuItem *> &v) 
        : SysMonitorMenuItem(l), title(t), options(v) {
        for(auto o: v) o->parent = this;
    }
    ~SysMonitorMenu() { 
        for(auto o: options) delete o;
    }
    void add(SysMonitorMenuItem *i) { options.push_back(i); i->parent = this; }
    void onSelect(SysMonitor *m) override;
};

void SysMonitorMenuItem::onKey(SysMonitor *m, int key) { 
    if (parent != NULL) parent->onKey(m, key); 
} 

class SysMonitorMenuPlaceholder : public SysMonitorMenuItem {
public:
    SysMonitorMenuPlaceholder(const string &t) : SysMonitorMenuItem(t) {
        selectable = false;
    } 
};

class SysMonitorPickOneChoice : virtual public SysMonitorMenuItem { 
public:
    SysMonitorPickOneChoice(const string &t) : SysMonitorMenuItem(t) {
        pickable = true;
    };
    void onSelect(SysMonitor *) override;
};

class SysMonitorPickOne : public SysMonitorMenu { 
public:
    function<void(const string &s)> onChange;
    SysMonitorPickOne(const string &t, const string &l, const string &def, 
        const vector<SysMonitorMenuItem *> &v, std::function<void(const string &)>f = NULL) 
        : SysMonitorMenu(t, l, v), onChange(f) { 
        value = def;
        label = l;
        text = label + " : " + value;
        pickable = true;
    } 
    void setValue(const string &s) {
        value = s;
        text = label + " : " + value;
        if (onChange != NULL) onChange(s);
    }
};

class SysMonitorMenuItemBoolean : public SysMonitorMenuItem {
public:
    bool value = false;
    function<void(bool)> onChange;
    SysMonitorMenuItemBoolean(const string &t, function<void(bool)>f = NULL) : SysMonitorMenuItem(t), onChange(f) {
        text = string("[ ] ") + t;
    }
    void onSelect(SysMonitor *m) {
        value = !value; 
        text[1] = value ? 'X' : ' ';
        if (onChange != NULL) onChange(value);
    }
};

class SysMonitorMenuItemText : virtual public SysMonitorMenuItem {
public:
    function<void(const string &)> onChange;
    SysMonitorMenuItemText(const string &t, const string &def, function<void(const string &)>f = NULL) : SysMonitorMenuItem(t), onChange(f) {
        label = t; 
        value = def;
        text = label + " : " + value;
    }
    void onSelect(SysMonitor *m) {}
    void onKey(SysMonitor *m, int key) override; 
    void setValue(const string & s) { 
        if (onChange != NULL) onChange(s);
    }
};

class PickOneChoiceEditable : public SysMonitorPickOneChoice, SysMonitorMenuItemText {
    public:
    PickOneChoiceEditable(const string &t, const string &def = "") 
        : SysMonitorPickOneChoice(t), SysMonitorMenuItemText(t, def) {
        pickable = true;
    }
    void onSelect(SysMonitor *) override {};
    void onKey(SysMonitor *, int) override;
};

class PickOneChoiceSubmenu : public SysMonitorPickOneChoice, SysMonitorPickOne{
    public:
    PickOneChoiceSubmenu(const string &t, const string &l, const string &def, 
        const vector<SysMonitorMenuItem *> &v, std::function<void(const string &)>f = NULL) 
        : SysMonitorPickOneChoice(l), SysMonitorPickOne(t, l, def, v, f) {
            pickable = true;
            title = t;
            label = l;
            text = label + " : " + value;
        }
    void onSelect(SysMonitor *m) override { SysMonitorMenu::onSelect(m); };
};

class MenuBack : public SysMonitorMenuItem {
public:
    MenuBack() : SysMonitorMenuItem("EXIT") {}
    void onSelect(SysMonitor *);
};

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

SysMonitorMenuItem *diskPicker(int n) { 
    return new SysMonitorPickOne(
            sfmt("CHOOSE DISK %d", n), sfmt("DISK %d   ", n), "<NONE>",
            {
                new SysMonitorPickOneChoice("<NONE>"),
                new PickOneChoiceSubmenu(sfmt("CHOOSE D%d: FLASH IMAGE", n), "FLASH DISK IMAGE", "", 
                    {                
                        new SysMonitorPickOneChoice("FLASH1.ATR"),
                        new SysMonitorPickOneChoice("FLASH2.ATR"),
                    }), 
                new PickOneChoiceSubmenu(sfmt("CHOOSE D%d: SMB IMAGE", n), "SMB DISK IMAGE", "",
                    {                
                        new SysMonitorMenuItemText("SEARCH SMB PATH", "//host/share/dir"),
                        new SysMonitorPickOneChoice("SMB1.ATR"),
                        new SysMonitorPickOneChoice("SMB2.ATR"),
                    }), 
                new PickOneChoiceSubmenu(sfmt("CHOOSE D%d: SMB DIR TO MOUNT", n), "SMB MOUNT", "", 
                    {                
                        new SysMonitorMenuItemText("SMB PATH", "//host/share/dir"),
                        new SysMonitorMenuItemBoolean("SPARTADOS 4.5 IMAGE"),
                        new SysMonitorMenuPlaceholder(""),
                        new SysMonitorMenuPlaceholder("PICK FROM AVAILABLE DIRS:"),
                        new SysMonitorPickOneChoice("DIR1"),
                        new SysMonitorPickOneChoice("DIR2"),
                    }), 
                new MenuBack(),
            }
    );
}

class SysMonitor {
    public:
    SysMonitorMenu rootMenu = SysMonitorMenu("MAIN MENU", "", {
        new SysMonitorPickOne(
            "CHOOSE CARTRIDGE IMAGE", "CARTRIDGE", "<NONE>",
            {
                new SysMonitorPickOneChoice("<NONE>"),
                new PickOneChoiceSubmenu("CHOOSE CART FLASH IMAGE", "FLASH IMAGE", "", 
                    {                
                        new SysMonitorPickOneChoice("FLASH1.CAR"),
                        new SysMonitorPickOneChoice("FLASH2.CAR"),
                    }), 
                new PickOneChoiceSubmenu("CHOOSE CART SMB IMAGE", "SMB IMAGE", "",
                    {                
                        new SysMonitorMenuItemText("SEARCH SMB PATH", "//host/share/dir"),
                        new SysMonitorPickOneChoice("SMB1.CAR"),
                        new SysMonitorPickOneChoice("SMB2.CAR"),
                    }), 
                new SysMonitorMenuPlaceholder(""),
                new MenuBack(),
            },
            [](const string &s){ printf("set cart to '%s'\n", s.c_str());}
        ), 
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
    });
    SysMonitorMenu *menu = &rootMenu;
    float activeTimeout = 0;
    bool exitRequested = false;
    uint8_t screenMem[24 * 40];
    void saveScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        for(int i = 0; i < sizeof(screenMem); i++) { 
            screenMem[i] = atariRam[savmsc + i];
        }
    }
    Debounce consoleDebounce = Debounce(240 * 1000 * 30);
    Debounce keyboardDebounce = Debounce(240 * 1000 * 30);

    void clearScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        for(int i = 0; i < sizeof(screenMem); i++) { 
            atariRam[savmsc + i] = 0;
         }
    }
    void drawScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        //atariRam[savmsc]++;
        //clearScreen();
        writeAt(-1, 2, DRAM_STR(" SYSTEM MONITOR "), true);
        writeAt(-1, 4, DRAM_STR("Everything will be fine!"), false);
        writeAt(-1, 5, sfmt(DRAM_STR("Timeout: %.0f"), activeTimeout), false);
        writeAt(-1, 7, sfmt(DRAM_STR("KBCODE = %02x CONSOL = %02x"), (int)pbiRequest->kbcode, (int)pbiRequest->consol), false);
        writeAt(-1, 9, menu->title, false);
        for(int i = 0; i < menu->options.size(); i++) {
            const int xpos = 0, ypos = 11; 
            const string cursor = DRAM_STR("-> ");
            writeAt(xpos, ypos + i, menu->selected == i ? cursor : DRAM_STR("   "), false);
            writeAt(xpos + cursor.length(), ypos + i, menu->options[i]->text + " ", menu->selected == i);
            writeAt(xpos + cursor.length() + menu->options[i]->text.length() + 1, ypos + i, "  ", false);
        }
        atariRam[712] = 255;
        atariRam[710] = 0;
    }
    void restoreScreen() { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        for(int i = 0; i < sizeof(screenMem); i++) { 
            atariRam[savmsc + i] = screenMem[i];
        }
        atariRam[712] = 0;
        atariRam[710] = 148;
    }
    void writeAt(int x, int y, const string &s, bool inv) { 
        uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
        if (x < 0) x = 20 - s.length() / 2;
        for(int i = 0; i < s.length(); i++) { 
            uint8_t c = s[i];
            if (c < 32) c += 64;
            else if (c < 96) c-= 32;
            atariRam[savmsc + y * 40 + x + i] = c + (inv ? 128 : 0);            
        }
    }
    void onKey(int key) {
        menu->options[menu->selected]->onKey(this, key);
    }
    void onConsoleKey(uint8_t key) {
        if (key != 7) activeTimeout = 60;
        if (key == 6) {
            do {
                menu->selected = min(menu->selected + 1, (int)menu->options.size() - 1);
            } while(menu->options[menu->selected]->selectable == false && menu->selected < menu->options.size() - 1);
        }
        if (key == 3) {
            do {
                menu->selected = max(menu->selected - 1, 0);
            } while(menu->options[menu->selected]->selectable == false && menu->selected > 0);
        }
        if (key == 5) menu->options[menu->selected]->onSelect(this);
        if (key == 0) exitRequested = true;
        if (key == 7 && exitRequested) activeTimeout = 0;
        //drawScreen();
    }
    public:
    PbiIocb *pbiRequest;
    uint32_t lastTsc;
    void pbi(PbiIocb *p) {
        pbiRequest = p;
        uint32_t tsc = XTHAL_GET_CCOUNT(); 
        if (activeTimeout <= 0) { // first reactivation, reinitialize 
            lastTsc = tsc;
            activeTimeout = 1.0;
            if (pbiRequest->consol == 0 || pbiRequest->kbcode == 0xe5)
                activeTimeout = 5.0;
            exitRequested = false;
            menu->selected = 0;
            keyboardDebounce.reset(pbiRequest->kbcode);
            consoleDebounce.reset(pbiRequest->consol);
            saveScreen();
            clearScreen();
            //drawScreen();
        }
        uint32_t elapsedTicks = tsc - lastTsc;
        lastTsc = tsc; 
        if (activeTimeout > 0) {
            activeTimeout -= elapsedTicks / 240000000.0;
            if (consoleDebounce.debounce(pbiRequest->consol, elapsedTicks)) { 
                onConsoleKey(pbiRequest->consol);
            }
            if (keyboardDebounce.debounce(pbiRequest->kbcode, elapsedTicks)) { 
                //drawScreen();
            }
            drawScreen();
            pbiRequest->result |= 0x80;
        }
        if (activeTimeout <= 0) {
            pbiRequest->result &= (~0x80);
            restoreScreen();  
            activeTimeout = 0;
        }
    }
} DRAM_ATTR sysMonitor;


void SysMonitorMenu::onSelect(SysMonitor *m) { 
    m->menu = this;
    m->clearScreen();
}

void MenuBack::onSelect(SysMonitor *m) { 
    m->menu = m->menu->parent;
    m->clearScreen();
}

void SysMonitorPickOneChoice::onSelect(SysMonitor *m) { 
    SysMonitorPickOne *p = ((SysMonitorPickOne *)parent);
    do {
        p->setValue(value);
        m->menu = m->menu->parent;
        p = (SysMonitorPickOne *)p->parent;
    } while(p->pickable == true);
    //m->menu = m->menu->parent;

    m->clearScreen();
}

void SysMonitorMenuItemText::onKey(SysMonitor *m, int key) {
    if (key == 127) { 
        if (!value.empty()) value.pop_back();
    } else if (key == 10 || key == 233) {
        setValue(value);
    } else {
        value += (char)key;
    }
    text = label + " : " + value;    
}

void PickOneChoiceEditable::onKey(SysMonitor *m, int key) {
    if (key == 127) { 
        if (!value.empty()) value.pop_back();
    } else if (key == 10 || key == 233) {
        setValue(value);
        SysMonitorPickOneChoice::onSelect(m);
    } else {
        value += (char)key;
    }
    text = label + " : " + value;    
}
