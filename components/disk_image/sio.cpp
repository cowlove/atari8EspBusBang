#ifdef CSIM
#include "csim.h"
#endif

#include "sio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "diskFlash.h"
#include "esp_heap_caps.h"
#include "smbNetwork.h"
#include "diskStitchProc.h"
#include "diskStitchSmb.h"

namespace {

// Whether there is PSRAM to copy ATR images into.
bool havePsram()
{
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0;
}

bool endsWithNoCase(const std::string &s, const char *suffix)
{
    const size_t n = strlen(suffix);
    return s.length() >= n && strcasecmp(s.c_str() + s.length() - n, suffix) == 0;
}

}  // namespace

Sio::~Sio()
{
    for (int d = 0; d < kDrives; ++d) {
        unmount(d);
    }
}

void Sio::setNetworkUp(void (*networkUp)())
{
    smbNetworkUp = networkUp;
    network_ = networkUp != nullptr;
}

void Sio::unmount(int drive)
{
    if (drive < 0 || drive >= kDrives) {
        return;
    }
    DiskImage *old = disks_[drive];
    disks_[drive] = nullptr;
    delete old;
}

bool Sio::mount(int drive, const std::string &diskSpec)
{
    if (drive < 0 || drive >= kDrives) {
        printf("Sio: no drive D%d:\n", drive + 1);
        return false;
    }
    unmount(drive);
    if (diskSpec.empty()) {
        return true;
    }

    DiskImage *image = nullptr;
    const char *unavailable = nullptr;
#ifdef CSIM
    if (endsWithNoCase(diskSpec, ".img")) {
        image = new DiskImageIMG(fs_, diskSpec.c_str());
    } else
#endif
    if (endsWithNoCase(diskSpec, ".atr.gz")) {
        if (havePsram()) {
            image = new DiskImageATR(fs_, diskSpec.c_str(), true);   // inflated into PSRAM
        } else {
            unavailable = "no PSRAM to inflate it into (a plain .atr works without)";
        }
    } else if (endsWithNoCase(diskSpec, ".atr")) {
        // Copied into PSRAM if there is some (writes then last until a
        // reboot); otherwise read and written in place on SPIFFS.
        image = new DiskImageATR(fs_, diskSpec.c_str(), havePsram());
    } else if (diskSpec.starts_with("spiffs:")) {
        image = new DiskImageStitchSPIFFS(fs_, diskSpec.c_str() + 7);
    } else if (diskSpec.starts_with("smb:")) {
        if (network_) {
            image = new DiskStitchGeneric<SmbConnection>(diskSpec.c_str());
        } else {
            unavailable = "no network in this app (setNetworkUp())";
        }
    } else if (diskSpec == "proc:") {
        if (procFsNodes_ != nullptr) {
            image = new DiskProcFs(*procFsNodes_);
        } else {
            unavailable = "no proc: files in this app (setProcFsNodes())";
        }
    }

    if (image == nullptr) {
        if (unavailable != nullptr) {
            printf("Sio: D%d: '%s': %s\n", drive + 1, diskSpec.c_str(), unavailable);
        } else {
            printf("Sio: D%d: unknown diskSpec '%s'\n", drive + 1, diskSpec.c_str());
        }
        return false;
    }
    disks_[drive] = image;
    if (!image->valid()) {
        printf("Sio: D%d: '%s' did not open\n", drive + 1, diskSpec.c_str());
        return false;
    }
    return true;
}

void Sio::reportStats()
{
    for (DiskImage *disk : disks_) {
        if (disk != nullptr) {
            disk->reportStats();
        }
    }
}
