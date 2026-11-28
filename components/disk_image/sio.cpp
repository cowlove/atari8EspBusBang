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
#ifndef CSIM
#include "esp_timer.h"   // use()'s timing
#endif
#include "smbNetwork.h"
#include "diskStitchProc.h"
#if DISK_IMAGE_SMB   // smbNetwork.h: main builds it only with A8BB_WIFI
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
    Pending *pending = pending_[drive];
    disks_[drive] = nullptr;
    pending_[drive] = nullptr;
    delete old;
    delete pending;
}

// The diskSpec's options, its kind and whether this app can open it: no
// file system access (open() resolves and checks a file).
bool Sio::parse(int drive, const std::string &diskSpec, Pending &out) const
{
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

    const char *unavailable = nullptr;
    bool known = true;
    bool isFile = true;   // a file or directory: resolved and checked
    Kind kind = Kind::Atr;
    const bool plainAtr = endsWithNoCase(path, ".atr");
    if (direct && !plainAtr) {
        unavailable = endsWithNoCase(path, ".atr.gz")
                          ? "direct needs an uncompressed .atr (a .atr.gz is inflated into PSRAM)"
                          : "direct is for .atr images only";
    } else
#ifdef CSIM
    if (endsWithNoCase(path, ".img")) {
        kind = Kind::Img;
    } else
#endif
    if (endsWithNoCase(path, ".atr.gz")) {
        kind = Kind::AtrGz;
        if (!havePsram()) {
            unavailable = "no PSRAM to inflate it into (a plain .atr works without)";
        }
    } else if (plainAtr) {
        kind = Kind::Atr;
    } else if (path.starts_with("spiffs:")) {
        kind = Kind::StitchDir;
        path = path.substr(7);
    } else if (path.starts_with("smb:")) {
        kind = Kind::Smb;
        isFile = false;
#if DISK_IMAGE_SMB
        if (!network_) {
            unavailable = "no network in this app (setNetworkUp())";
        }
#else
        unavailable = "no SMB in this build (main without A8BB_WIFI; use sidecar!smb:)";
#endif
    } else if (path == "proc:") {
        kind = Kind::Proc;
        isFile = false;
        if (procFsNodes_ == nullptr) {
            unavailable = "no proc: files in this app (setProcFsNodes())";
        }
    } else {
        known = false;
    }

    if (!known) {
        LOG("Sio: D%d: unknown diskSpec '%s'\n", drive + 1, diskSpec.c_str());
        return false;
    }
    if (unavailable != nullptr) {
        LOG("Sio: D%d: '%s': %s\n", drive + 1, diskSpec.c_str(), unavailable);
        return false;
    }
    out.diskSpec = diskSpec;
    out.path = path;
    out.kind = kind;
    out.direct = direct;
    out.isFile = isFile;
    return true;
}

bool Sio::open(int drive, const Pending &parsed)
{
    Pending spec = parsed;
    if (spec.isFile) {
        spec.path = resolve(spec.path);
        struct stat info;
        if (stat(spec.path.c_str(), &info) != 0) {
            LOG("Sio: D%d: '%s': not found\n", drive + 1, spec.diskSpec.c_str());
            return false;
        }
    }
    DiskImage *image = nullptr;
    switch (spec.kind) {
#ifdef CSIM
    case Kind::Img:
        image = new DiskImageIMG(spec.path.c_str());
        break;
#else
    case Kind::Img:
        break;
#endif
    case Kind::AtrGz:
        image = new DiskImageATR(spec.path.c_str(), true);   // inflated into PSRAM
        break;
    case Kind::Atr:
        // Copied into PSRAM if there is some and not direct (writes then
        // last until a reboot); otherwise read and written in place.
        image = new DiskImageATR(spec.path.c_str(), havePsram() && !spec.direct);
        break;
    case Kind::StitchDir:
        image = new DiskImageStitchDir(spec.path.c_str());
        break;
    case Kind::Smb:
#if DISK_IMAGE_SMB
        image = new DiskStitchGeneric<SmbConnection>(spec.path.c_str());
#endif
        break;
    case Kind::Proc:
        image = new DiskProcFs(*procFsNodes_);
        break;
    }
    if (image == nullptr) {
        LOG("Sio: D%d: '%s': not available\n", drive + 1, spec.diskSpec.c_str());
        return false;
    }
    disks_[drive] = image;
    if (!image->valid()) {
        LOG("Sio: D%d: '%s' did not open\n", drive + 1, spec.diskSpec.c_str());
        return false;
    }
    return true;
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
    Pending *spec = new Pending;
    if (!parse(drive, diskSpec, *spec)) {
        delete spec;
        return false;
    }
    if (mountOnUse_) {
        pending_[drive] = spec;
        LOG("Sio: D%d: '%s': mounted; opens on first use\n", drive + 1, diskSpec.c_str());
        return true;
    }
    const bool ok = open(drive, *spec);
    delete spec;
    return ok;
}

DiskImage *Sio::use(int drive)
{
    if (drive < 0 || drive >= kDrives) {
        return nullptr;
    }
    if (Pending *spec = pending_[drive]) {
        pending_[drive] = nullptr;
        const int64_t start = esp_timer_get_time();
        const bool ok = open(drive, *spec);
        LOG("Sio: D%d: '%s': %s on first use (%d ms)\n", drive + 1, spec->diskSpec.c_str(),
            ok ? "opened" : "failed", (int)((esp_timer_get_time() - start) / 1000));
        delete spec;
    }
    return disks_[drive];
}

void Sio::reportStats()
{
    for (DiskImage *disk : disks_) {
        if (disk != nullptr) {
            disk->reportStats();
        }
    }
}
