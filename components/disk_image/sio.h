#pragma once

// Sio: the Atari's SIO disk drives D1: through D8:, shared by the host app
// (main/) and the Sidecar app (sidecar/).
//
// It owns the eight drives: mount() parses a diskSpec (the string that
// says what a drive holds) and creates the DiskImage for it. Later it will
// also take the SIO DCB handling that main/pbi.cpp does today, so the host
// can run SIO itself or hand the DCB to the Sidecar, which answers it with
// the same code.
//
// diskSpec syntax (suffixes are case-insensitive):
//   "/name.atr"                   ATR image: copied into PSRAM, or without
//                                 PSRAM, used in place (writes persist)
//   "/name.atr.gz"                gzipped ATR image, inflated into PSRAM (needs PSRAM)
//   "spiffs:<dir>"                SDFS image stitched from a directory's files
//   "smb://server/share[/...]"    SDFS image stitched from an SMB share;
//                                 needs setNetworkUp()
//   "proc:"                       DOS disk of function files; needs
//                                 setProcFsNodes()
//   "/name.img"                   raw hard-disk image (csim only)
//   ""                            empty drive
// Every app parses the same syntax. What differs per app is only what the
// app supplies: its network and its proc: files.
//
// Paths go through the VFS, looked up in each of roots() in turn (see
// resolve()): on the boards "/name.atr" is /spiffs/name.atr, and a path on
// another mount, such as the Sidecar's SD card ("/sd/name.atr"), is used as
// given; in csim, a host path, then the repo's spiffs/ directory.

#include <string>
#include <vector>

#include "diskImage.h"

struct ProcFsFile;   // diskStitchProc.h

class Sio {
public:
    static constexpr int kDrives = 8;

    ~Sio();

    // What the diskSpecs refer to. Set these before mount().
    void setNetworkUp(void (*networkUp)());                    // "smb:"
    void setProcFsNodes(const std::vector<ProcFsFile> *nodes)  // "proc:"
    {
        procFsNodes_ = nodes;
    }

    // Mount diskSpec in drive (0 = D1:), unmounting (deleting) any image
    // already there. An empty diskSpec just empties the drive. False, with a
    // message printed, if the diskSpec is not understood or not available
    // in this app, or the image is not valid; an invalid image stays mounted
    // anyway (it reads as not ready).
    bool mount(int drive, const std::string &diskSpec);
    void unmount(int drive);

    // The image in drive (0 = D1:), or nullptr. Safe from IRAM code: one load.
    DiskImage *disk(int drive) const { return drive >= 0 && drive < kDrives ? disks_[drive] : nullptr; }

    void reportStats();

    // Where a diskSpec's path is looked for, in order; "" is the path as
    // given. The default: {"/spiffs", ""}, or {"", "../../spiffs"} in csim.
    void setRoots(std::vector<std::string> roots) { roots_ = std::move(roots); }
    const std::vector<std::string> &roots() const { return roots_; }
    // path under the first root where it exists, or under the first root.
    std::string resolve(const std::string &path) const;

private:
#ifdef CSIM
    std::vector<std::string> roots_ = {"", "../../spiffs"};
#else
    std::vector<std::string> roots_ = {"/spiffs", ""};
#endif
    bool network_ = false;
    const std::vector<ProcFsFile> *procFsNodes_ = nullptr;
    DiskImage *disks_[kDrives] = {};
};
