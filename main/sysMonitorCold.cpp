// The system monitor's cold-bus helpers: everything that uses std:: or does
// I/O, kept out of sysMonitor.cpp, whose code runs from internal RAM and can
// run hot. Nothing here may be called from SysMonitor::pbi(); see
// sysMonitor.h.
#ifdef CSIM
#include "csim.h"
#else
#include "sdkconfig.h"
#endif
#if CONFIG_A8BB_SYSMONITOR   // the whole file

#include "sysMonitor.h"
#include "sysConfig.h"
#include "cartridge.h"
#include "hostSio.h"
#include "mmu.h"

extern AtariCart atariCart;
vector<string> listFiles(const char *directory, const char *pattern);   // diskFlash.cpp

// The cartridge images on SPIFFS, for the menu (at boot).
void sysMonitorListCarts(void (*add)(const char *name, void *ctx), void *ctx)
{
    vector<string> files = listFiles("/spiffs", "*.CAR");
    vector<string> roms = listFiles("/spiffs", "*.ROM");
    files.insert(std::end(files), std::begin(roms), std::end(roms));
    for (const string &f : files)
        add(f.c_str() + 1, ctx);   // without the leading '/'
}

// A cartridge picked in the monitor: save it and switch to it.
void sysMonitorSetCart(const char *name)
{
    printf("set cart to '%s'\n", name);
    config.cartImage = string("/") + name;
    config.save();
    atariCart.open(sio.resolve(config.cartImage).c_str());
    mmuOnChange(/*force=*/true);
}

#endif   // CONFIG_A8BB_SYSMONITOR
