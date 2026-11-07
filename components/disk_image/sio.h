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
//   "/name.atr", "/name.atr.gz"   ATR image on SPIFFS (.gz inflated into PSRAM)
//   "spiffs:<dir>"                SDFS image stitched from a SPIFFS directory
//   "smb://server/share[/...]"    SDFS image stitched from an SMB share;
//                                 needs setNetworkUp()
//   "proc:"                       DOS disk of function files; needs
//                                 setProcFsNodes()
//   "/name.img"                   raw hard-disk image (csim only)
//   ""                            empty drive
// Every app parses the same syntax. What differs per app is only what the
// app supplies: its SPIFFS volume, its network and its proc: files.

#include <string>
#include <vector>

#include "diskImage.h"
#include "spiffs.h"

struct ProcFsFile;   // diskStitchProc.h

class Sio {
public:
    static constexpr int kDrives = 8;

    ~Sio();

    // What the diskSpecs refer to. Set these before mount().
    void setFs(spiffs *fs) { fs_ = fs; }                       // SPIFFS images
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

private:
    spiffs *fs_ = nullptr;
    bool network_ = false;
    const std::vector<ProcFsFile> *procFsNodes_ = nullptr;
    DiskImage *disks_[kDrives] = {};
};
