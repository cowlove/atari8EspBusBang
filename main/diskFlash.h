#pragma once
#include <inttypes.h>
#include <stddef.h>
#include <string>

#include "spiffs.h"
#include "diskImage.h"

using std::string;

class DiskImageATR : public DiskImage {
public:
    string filename;
    AtrImageHeader header;
    uint8_t *image = nullptr;
    uint8_t *imageAllocation = nullptr;
    spiffs *fs;
    spiffs_file fd = -1;

    DiskImageATR(spiffs *fs, const char *f, bool cache) : fs(fs) { (void)fs; open(f, cache); }
    void close();
    void open(const char *f, bool cacheInPsram);
    bool valid();
    int sectorSize();   
    int sectorCount();
    size_t read(uint8_t *buf, size_t sector);
    int write(const uint8_t *buf, size_t sector);
};

#ifdef CSIM
class DiskImageIMG : public DiskImage {
public:
    string filename;
    uint8_t *image = nullptr;
    size_t imageBytes = 0;

    DiskImageIMG(spiffs *fs, const char *f) { (void)fs; open(fs, f); }
    void open(spiffs *spiffs_fs, const char *f);
    bool valid() override;
    int sectorSize() override;
    int sectorCount() override;
    size_t read(uint8_t *buf, size_t sector) override;
    int write(const uint8_t *buf, size_t sector) override;
    int specificSectorSize(int sector) override;
    int offsetToSector(int offset) override;
    int sectorToOffset(int sector) override;
    size_t imageSize() override;
};
#endif


// TODO: move to DiskSmb.h 

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
