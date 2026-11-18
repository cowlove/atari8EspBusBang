#ifdef CSIM
#include "csim.h"
#endif

#include "a8log.h"
#include "sio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "diskFlash.h"
#include "esp_heap_caps.h"
#include "smbNetwork.h"
#include "diskStitchProc.h"
#if DISK_IMAGE_SMB   // smbNetwork.h: not in main's builds
#include "diskStitchSmb.h"
#endif

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

std::string join(const std::string &root, const std::string &path)
{
    if (root.empty()) return path;
    return path.starts_with("/") ? root + path : root + "/" + path;
}

}  // namespace

std::string Sio::resolve(const std::string &path) const
{
    if (path.empty()) return path;   // "no file" stays that way
    for (const std::string &root : roots_) {
        const std::string full = join(root, path);
        struct stat info;
        if (stat(full.c_str(), &info) == 0) return full;
    }
    return roots_.empty() ? path : join(roots_.front(), path);
}

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
        LOG("Sio: no drive D%d:\n", drive + 1);
        return false;
    }
    unmount(drive);
    if (diskSpec.empty()) {
        return true;
    }

    // "path,option,...": the mount options after the first comma (sio.h).
    std::string path = diskSpec;
    bool direct = false;
    if (const size_t comma = diskSpec.find(','); comma != std::string::npos) {
        path = diskSpec.substr(0, comma);
        size_t start = comma + 1;
        while (start <= diskSpec.size()) {
            size_t end = diskSpec.find(',', start);
            if (end == std::string::npos) end = diskSpec.size();
            const std::string option = diskSpec.substr(start, end - start);
            if (option == "direct") {
                direct = true;
            } else if (!option.empty()) {
                LOG("Sio: D%d: '%s': unknown mount option '%s'\n", drive + 1, diskSpec.c_str(),
                    option.c_str());
                return false;
            }
            start = end + 1;
        }
    }

    DiskImage *image = nullptr;
    const char *unavailable = nullptr;
    const bool plainAtr = endsWithNoCase(path, ".atr");
    if (direct && !plainAtr) {
        unavailable = endsWithNoCase(path, ".atr.gz")
                          ? "direct needs an uncompressed .atr (a .atr.gz is inflated into PSRAM)"
                          : "direct is for .atr images only";
    } else
#ifdef CSIM
    if (endsWithNoCase(path, ".img")) {
        image = new DiskImageIMG(resolve(path).c_str());
    } else
#endif
    if (endsWithNoCase(path, ".atr.gz")) {
        if (havePsram()) {
            image = new DiskImageATR(resolve(path).c_str(), true);   // inflated into PSRAM
        } else {
            unavailable = "no PSRAM to inflate it into (a plain .atr works without)";
        }
    } else if (plainAtr) {
        // Copied into PSRAM if there is some and not direct (writes then
        // last until a reboot); otherwise read and written in place.
        image = new DiskImageATR(resolve(path).c_str(), havePsram() && !direct);
    } else if (path.starts_with("spiffs:")) {
        image = new DiskImageStitchDir(resolve(path.substr(7)).c_str());
    } else if (path.starts_with("smb:")) {
#if DISK_IMAGE_SMB
        if (network_) {
            image = new DiskStitchGeneric<SmbConnection>(path.c_str());
        } else {
            unavailable = "no network in this app (setNetworkUp())";
        }
#else
        unavailable = "no SMB in this app (DISK_IMAGE_SMB=0: main; use sidecar!smb:)";
#endif
    } else if (path == "proc:") {
        if (procFsNodes_ != nullptr) {
            image = new DiskProcFs(*procFsNodes_);
        } else {
            unavailable = "no proc: files in this app (setProcFsNodes())";
        }
    }

    if (image == nullptr) {
        if (unavailable != nullptr) {
            LOG("Sio: D%d: '%s': %s\n", drive + 1, diskSpec.c_str(), unavailable);
        } else {
            LOG("Sio: D%d: unknown diskSpec '%s'\n", drive + 1, diskSpec.c_str());
        }
        return false;
    }
    disks_[drive] = image;
    if (!image->valid()) {
        LOG("Sio: D%d: '%s' did not open\n", drive + 1, diskSpec.c_str());
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
