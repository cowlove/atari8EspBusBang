#pragma once
#include <inttypes.h>
#include <stddef.h>
#include <limits.h>
#include <string>

#include "spiffs.h"
#include "diskImage.h"
#include "diskStitch.h"

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

class SpiffsStorage : public StorageInterface {
    spiffs *fs;
    spiffs_file fd = -1;
    string root;

    string fullPath(const char *path) const {
        string relative = path == nullptr ? "" : path;
        while (!relative.empty() && relative[0] == '/') relative.erase(0, 1);
        if (root == "/") return "/" + relative;
        return relative.empty() ? root : root + "/" + relative;
    }

    static int ioSize(size_t size) {
        return size <= static_cast<size_t>(INT_MAX) ? static_cast<int>(size) : -1;
    }

public:
    SpiffsStorage(spiffs *fs, const char *directory = "") : fs(fs), root(directory ? directory : "") {}

    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)> callback) override {
        string directory = fullPath(path);
        spiffs_DIR dir;
        if (SPIFFS_opendir(fs, directory.c_str(), &dir) == nullptr) return -EIO;

        int count = 0;
        spiffs_dirent entry;
        while (SPIFFS_readdir(&dir, &entry) != nullptr) {
            if (entry.type != SPIFFS_TYPE_FILE) continue;
            struct dirent hostEntry = {};
            size_t nameLength = strnlen(reinterpret_cast<const char *>(entry.name), sizeof(entry.name));
            nameLength = std::min(nameLength, sizeof(hostEntry.d_name) - 1);
            memcpy(hostEntry.d_name, entry.name, nameLength);
            hostEntry.d_name[nameLength] = '\0';
            hostEntry.d_type = DT_REG;
            callback(count++, &hostEntry, entry.size);
        }
        SPIFFS_closedir(&dir);
        return count;
    }

    int open(const char *path, int mode, int permissions = 0) override {
        if (fd >= 0) close();

        int flags = (mode & O_ACCMODE) == O_RDONLY ? SPIFFS_O_RDONLY : SPIFFS_O_RDWR;
        if (mode & O_CREAT) flags |= SPIFFS_O_CREAT;
        if (mode & O_TRUNC) flags |= SPIFFS_O_TRUNC;
        fd = SPIFFS_open(fs, fullPath(path).c_str(), flags, permissions);
        return fd < 0 ? -ENOENT : fd;
    }

    int pwrite(const uint8_t *buffer, size_t length, size_t position, bool) override {
        int size = ioSize(length);
        if (fd < 0) return -EBADF;
        if (size < 0 || position > static_cast<size_t>(INT_MAX)) return -EOVERFLOW;
        if (SPIFFS_lseek(fs, fd, static_cast<int>(position), SPIFFS_SEEK_SET) < 0) return -EIO;
        int written = SPIFFS_write(fs, fd, const_cast<uint8_t *>(buffer), size);
        return written == size ? written : written < 0 ? written : -EIO;
    }

    int pread(uint8_t *buffer, size_t length, size_t position) override {
        int size = ioSize(length);
        if (fd < 0) return -EBADF;
        if (size < 0 || position > static_cast<size_t>(INT_MAX)) return -EOVERFLOW;
        if (SPIFFS_lseek(fs, fd, static_cast<int>(position), SPIFFS_SEEK_SET) < 0) return -EIO;
        return SPIFFS_read(fs, fd, buffer, size);
    }

    int remove(const char *path) override {
        return SPIFFS_remove(fs, fullPath(path).c_str()) < 0 ? -EIO : 0;
    }

    int rename(const char *oldPath, const char *newPath) override {
        return SPIFFS_rename(fs, fullPath(oldPath).c_str(), fullPath(newPath).c_str()) < 0 ? -EIO : 0;
    }

    int truncate(const char *path, size_t length) override {
        if (length > UINT32_MAX) return -EOVERFLOW;
        spiffs_file file = SPIFFS_open(fs, fullPath(path).c_str(), SPIFFS_O_RDWR, 0);
        if (file < 0) return -ENOENT;
        int result = SPIFFS_ftruncate(fs, file, static_cast<uint32_t>(length));
        int closeResult = SPIFFS_close(fs, file);
        return result < 0 || closeResult < 0 ? -EIO : 0;
    }

    void close() override {
        if (fd >= 0) {
            SPIFFS_close(fs, fd);
            fd = -1;
        }
    }
};

class DiskImageStitchSPIFFS : public DiskStitchImage {
    SpiffsStorage storage;

public:
    explicit DiskImageStitchSPIFFS(spiffs *fs, const char *directory = "/")
        : DiskStitchImage(&storage), storage(fs, directory) {}
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
