#include "sysMonitor.h"
#include "asmDefs.h"
#include "cartridge.h"
#include "spiffs.h" 

extern uint8_t atariRam[];
extern uint8_t pbiROM[];
struct spiffs_t; 
extern struct spiffs_t *spiffs_fs;
extern int sysMonitorTime; 
extern AtariCart atariCart;
extern int interruptTicks;
vector<string> spiffsDir(struct spiffs_t *fs, const char *d, const char *pat, bool icase); 
void mmuOnChange(bool force = false);

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

SysMonitorMenuItem *cartridgePicker() { 
    vector<string> files = spiffsDir(spiffs_fs, "/", "*.CAR", true);
    vector<string> roms = spiffsDir(spiffs_fs, "/", "*.ROM", true);
    files.insert(std::end(files), std::begin(roms), std::end(roms));
    vector<SysMonitorMenuItem *> cartPicks;
    for(auto f : files) {
        cartPicks.push_back(new SysMonitorPickOneChoice(f.c_str() + 1)); 
    }
    return new SysMonitorPickOne(
        "SELECTED CARTRIDGE IMAGE", "CARTRIDGE", "<NONE>",
        {
            new SysMonitorPickOneChoice("<NO CARTRIDGE>"),
            new PickOneChoiceSubmenu("CHOOSE CART FLASH IMAGE", "FLASH IMAGE", "", cartPicks), 
            new PickOneChoiceSubmenu("CHOOSE CART SMB IMAGE", "SMB IMAGE", "",
                {                
                    new SysMonitorMenuItemText("SEARCH SMB PATH", "//host/share/dir"),
                    new SysMonitorPickOneChoice("SMB1.CAR"),
                    new SysMonitorPickOneChoice("SMB2.CAR"),
                }), 
            new SysMonitorMenuPlaceholder(""),
            new MenuBack(),
        },
        [](const string &s){ 
            printf("set cart to '%s'\n", s.c_str());
            config.cartImage = string("/") + s;
            config.save();
            atariCart.open(spiffs_fs, config.cartImage.c_str());
            mmuOnChange(/*force=*/true);
        }
    );
}
void SysMonitor::saveScreen() { 
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    for(int i = 0; i < sizeof(screenMem); i++) { 
        screenMem[i] = atariRam[savmsc + i];
    }
}
void SysMonitor::clearScreen() { 
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    for(int i = 0; i < sizeof(screenMem); i++) { 
        atariRam[savmsc + i] = 0;
    }
}
void SysMonitor::restoreScreen() { 
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    for(int i = 0; i < sizeof(screenMem); i++) { 
        atariRam[savmsc + i] = screenMem[i];
    }
    atariRam[712] = 0;
    atariRam[710] = 148;
}
void SysMonitor::writeAt(int x, int y, const string &s, bool inv) { 
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    if (x < 0) x = 20 - s.length() / 2;
    for(int i = 0; i < s.length(); i++) { 
        uint8_t c = s[i];
        if (c < 32) c += 64;
        else if (c < 96) c-= 32;
        atariRam[savmsc + y * 40 + x + i] = c + (inv ? 128 : 0);            
    }
}
void SysMonitor::onKey(int key) {
    menu->options[menu->selected]->onKey(this, key);
}

void SysMonitor::onConsoleKey(uint8_t key) {
    if (key != 7) activeTimeout = 600;
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
    if (key == 0 || key == 1) exitRequested = true;
    if (key == 7 && exitRequested) activeTimeout = 0;
    //drawScreen();
}

SysMonitor::SysMonitor() 
    : rootMenu("MAIN MENU", "", {
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
    new SysMonitorMenuItemText("START MONITOR EVERY N SECONDS", "10", [](const string &v) { 
        sscanf(v.c_str(), "%d", &sysMonitorTime); 
    }),
    new SysMonitorMenuItemText("PBI INTERRUPTS PER SECOND", "10", [](const string &v) {
        int intPerSec; 
        if (sscanf(v.c_str(), "%d", &intPerSec) == 1 && intPerSec > 0)
            interruptTicks = 240 * 1001 * 1001 / intPerSec; 
    }),
    new SysMonitorMenuPlaceholder(""),
    new SysMonitorMenuItemRadioButton("OPT 3"),
    new SysMonitorMenuItemRadioButton("OPT 4"),
    new SysMonitorMenuItemRadioButton("OPT 5"),
    }) {

}

void SysMonitor::drawScreen() { 
    //uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
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
        string text = menu->options[i]->text;
        writeAt(xpos + cursor.length(), ypos + i, text, menu->selected == i);
        bool cursorOn = menu->selected == i && menu->options[i]->showCursor == true && (XTHAL_GET_CCOUNT() & 0x4000000) == 0; 
        writeAt(xpos + cursor.length() + text.length(), ypos + i, " ", cursorOn);
        writeAt(xpos + cursor.length() + menu->options[i]->text.length() + 1, ypos + i, "  ", false);
    }
    atariRam[712] = 255;
    atariRam[710] = 0;
}

void SysMonitor::pbi(PbiIocb *p) {
    pbiRequest = p;
    uint32_t tsc = XTHAL_GET_CCOUNT(); 
    if (activeTimeout <= 0) { // first reactivation, reinitialize 
        lastTsc = tsc;
        activeTimeout = 2.0;
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
            // TODO: need to do keypressToAtascii[]
            //drawScreen();
        }
        drawScreen();
        pbiRequest->result |= RES_FLAG_MONITOR;
    }
    if (activeTimeout <= 0) {
        pbiRequest->result &= (~RES_FLAG_MONITOR);
        restoreScreen();  
        activeTimeout = 0;
    }
    pbiRequest->result |= RES_FLAG_COPYOUT;
    uint16_t savmsc = (atariRam[89] << 8) + atariRam[88];
    int len = 40 * 24;
    for(int i = 0; i < len; i++)
        pbiROM[0x400 + i] = atariRam[savmsc + i]; // TODO get proper paddr for this 
    pbiRequest->copybuf = savmsc;
    pbiRequest->copylen = len; 
}

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

void SysMonitorMenuItem::onKey(SysMonitor *m, int key) { 
    if (parent != NULL) parent->onKey(m, key); 
} 

SysMonitor *sysMonitor = NULL;
SysConfig config;

void SysConfig::load() {
    spiffs_file fd = SPIFFS_open(spiffs_fs, "/config.txt", SPIFFS_O_RDONLY, 0);
    char buf[128] = {0};
    SPIFFS_read(spiffs_fs, fd, buf, sizeof(buf));
    cartImage = buf;
    SPIFFS_close(spiffs_fs, fd);
}
void SysConfig::save() {
    spiffs_file fd = SPIFFS_open(spiffs_fs, "/config.txt", SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_RDWR, 0);
    SPIFFS_write(spiffs_fs, fd, (void *)cartImage.c_str(), cartImage.length());
    SPIFFS_close(spiffs_fs, fd);
}