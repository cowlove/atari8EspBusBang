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
    uint8_t *image;
    spiffs *fs;
    spiffs_file fd;

    DiskImageATR(spiffs *spiffs_fs, const char *f, bool cache) : fs(spiffs_fs) { open(f, cache); }
    void close();
    void open(const char *f, bool cacheInPsram);
    bool valid();
    int sectorSize();   
    int sectorCount();
    size_t read(uint8_t *buf, size_t sector);
    size_t write(const uint8_t *buf, size_t sector);
};


// TODO: move to DiskSmb.h 

#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"

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

    DiskImageSMB(spiffs *spiffs_fs, const char *f, bool cache) { open(f, cache); }
    void close();
    void open(const char *f, bool cacheInPsram);
    bool valid();
    int sectorSize();    
    int sectorCount();
    size_t read(uint8_t *buf, size_t sector);
    size_t write(const uint8_t *buf, size_t sector);
};

