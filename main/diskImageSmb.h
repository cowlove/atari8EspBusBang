#pragma once
// DiskImageSMB: an ATR image on an SMB share (host app only: needs libsmb2
// and wifi). Not instantiated anywhere yet.
#include <string>

#include "diskImage.h"
#include "spiffs.h"

using std::string;

#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"

#undef TAG
#define TAG "smb"
#define CONFIG_SMB_USER "guest"
#define CONFIG_SMB_HOST "miner6.local"
#define CONFIG_SMB_PATH "pub"
#include "lwip/sys.h"

class DiskImageSMB : public DiskImage {
public:
    string filename;
    AtrImageHeader header;
    struct smb2_context *smb2;
    struct smb2_url *url;
    struct smb2fh *fh;

    DiskImageSMB(spiffs *fs, const char *f, bool cache) { (void)fs; open(f, cache); }
    void close();
    void open(const char *f, bool cacheInPsram);
    bool valid();
    int sectorSize();    
    int sectorCount();
    size_t read(uint8_t *buf, size_t sector);
    int write(const uint8_t *buf, size_t sector);
};
