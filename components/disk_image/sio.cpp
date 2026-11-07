#include "sio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "diskFlash.h"
#include "smbNetwork.h"
#include "diskStitchProc.h"
#include "diskStitchSmb.h"

namespace {

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
    if (endsWithNoCase(diskSpec, ".atr") || endsWithNoCase(diskSpec, ".atr.gz")) {
        image = new DiskImageATR(fs_, diskSpec.c_str(), true);
    } else if (diskSpec.starts_with("spiffs:")) {
        image = new DiskImageStitchSPIFFS(fs_, diskSpec.c_str() + 7);
    } else if (diskSpec.starts_with("smb:")) {
        if (network_) {
            image = new DiskStitchGeneric<SmbConnection>(diskSpec.c_str());
        } else {
            unavailable = "no network (setNetworkUp())";
        }
    } else if (diskSpec == "proc:") {
        if (procFsNodes_ != nullptr) {
            image = new DiskProcFs(*procFsNodes_);
        } else {
            unavailable = "no proc: files (setProcFsNodes())";
        }
    }

    if (image == nullptr) {
        if (unavailable != nullptr) {
            printf("Sio: D%d: '%s': %s in this app\n", drive + 1, diskSpec.c_str(), unavailable);
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

void Sio::mountAll(const std::string (&diskSpecs)[kDrives])
{
    for (int d = 0; d < kDrives; ++d) {
        mount(d, diskSpecs[d]);
    }
}

void Sio::reportStats()
{
    for (DiskImage *disk : disks_) {
        if (disk != nullptr) {
            disk->reportStats();
        }
    }
}
