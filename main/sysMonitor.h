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

#ifndef CSIM
#include "xtensa/core-macros.h"
#endif

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
    bool showCursor = false;
    bool group = false;
};

class SysMonitorMenu : virtual public SysMonitorMenuItem {
public:
    string title;
    vector<SysMonitorMenuItem *> options;
    int selected = 0;
    SysMonitorMenu() {}
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

class SysMonitorMenuItemRadioButton : public SysMonitorMenuItemBoolean {
public:
    bool value = false;
    function<void(bool)> onChange;
    SysMonitorMenuItemRadioButton(const string &t, function<void(bool)>f = NULL) : SysMonitorMenuItemBoolean(t, f) {
        text = string("[ ] ") + t;
        group = true;
    }
    void onSelect(SysMonitor *m) {
       // if (value == true)
       //     return;

        for(int i = parent->selected - 1; i >= 0 && parent->options[i]->group == true; i--) {
            parent->options[i]->value = false;
            parent->options[i]->text[1] = ' ';
        }
        for(int i = parent->selected + 1; i < parent->options.size() && parent->options[i]->group == true; i++) {
            parent->options[i]->value = false;
            parent->options[i]->text[1] = ' ';
        }
        text[1] = 'X';
        value = true; 
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
        showCursor = true;
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
        showCursor = true;
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

SysMonitorMenuItem *diskPicker(int n);

class SysMonitor {
    public:
    SysMonitorMenu rootMenu;
    SysMonitorMenu *menu = &rootMenu;
    float activeTimeout = 0;
    bool exitRequested = false;
    uint8_t screenMem[24 * 40];
    void saveScreen(); 
    Debounce consoleDebounce = Debounce(240 * 1000 * 30);
    Debounce keyboardDebounce = Debounce(240 * 1000 * 30);

    void drawScreen(); 
    void clearScreen();
    void restoreScreen();
    void writeAt(int x, int y, const string &s, bool inv);
    void onKey(int key);
    void onConsoleKey(uint8_t key);
    public:
    PbiIocb *pbiRequest;
    uint32_t lastTsc;
    SysMonitor();
    void pbi(PbiIocb *p);
};

extern SysMonitor *sysMonitor;

class SysConfig { 
public:
    string cartImage;
    string diskSpec[8];
    int ioTimeoutSec = 120, wdTimeoutSec = 120; 
    uint16_t wdMemLoc = 0x600;
    int irqFreq = 10;
    void save();
    void load();
}; 

extern SysConfig config;
