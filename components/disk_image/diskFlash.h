#pragma once
#include <inttypes.h>
#include <stddef.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diskImage.h"
#include "diskStitchImageSDFS.h"

using std::string;

// Files are reached through the VFS (open(), read() ...): "/spiffs/..." on
// the boards, any other mount such as "/sd/...", or host paths in csim.
// Sio resolves a diskSpec to the path these take (Sio::resolve()).

// The names, with a leading '/', of the files in directory that match the
// wildcard pattern (* and ?, case-insensitive).
std::vector<string> listFiles(const char *directory, const char *pattern);

class DiskImageATR : public DiskImage {
public:
    string filename;
    AtrImageHeader header;
    uint8_t *image = nullptr;
    uint8_t *imageAllocation = nullptr;
    int fd = -1;

    DiskImageATR(const char *path, bool cache) { open(path, cache); }
    // Sio::unmount() deletes images on every remount (the Sidecar gets a
    // Mount for each drive on every host boot): free the PSRAM copy or
    // close the file.
    ~DiskImageATR() override { close(); }
    void close();
    void open(const char *path, bool cacheInPsram);
    bool valid();
    int sectorSize();   
    int sectorCount();
    size_t read(uint8_t *buf, size_t sector);
    int write(const uint8_t *buf, size_t sector);
};

// The files of one directory, for the SDFS stitch image.
class DirStorage : public CachedStorageInterface {
    string root;

    string fullPath(const char *path) const {
        string relative = path == nullptr ? "" : path;
        while (!relative.empty() && relative[0] == '/') relative.erase(0, 1);
        if (root == "/") return "/" + relative;
        return relative.empty() ? root : root + "/" + relative;
    }

    static int decodeHandle(uintptr_t handle) {
        return static_cast<int>(handle - 1);
    }

public:
    explicit DirStorage(const char *directory = "") : root(directory ? directory : "") {
        while (root.length() > 1 && root.back() == '/') root.pop_back();
    }

    ~DirStorage() override {
        cacheCloseAll();
    }

    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)> callback) override {
        string directory = fullPath(path);
        DIR *dir = ::opendir(directory.c_str());
        if (dir == nullptr) return -EIO;

        int count = 0;
        while (struct dirent *entry = ::readdir(dir)) {
            string file = directory + "/" + entry->d_name;
            struct stat info;
            if (::stat(file.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
            struct dirent hostEntry = {};
            strncpy(hostEntry.d_name, entry->d_name, sizeof(hostEntry.d_name) - 1);
            hostEntry.d_type = DT_REG;
            callback(count++, &hostEntry, static_cast<size_t>(info.st_size));
        }
        ::closedir(dir);
        return count;
    }

protected:
    uintptr_t openHandle(const char *path, int mode, int permissions) override {
        int fd = ::open(fullPath(path).c_str(), mode, permissions);
        return fd < 0 ? 0 : static_cast<uintptr_t>(fd) + 1;
    }

    void closeHandle(uintptr_t handle) override {
        ::close(decodeHandle(handle));
    }

    int pwriteHandle(uintptr_t handle, const uint8_t *buffer, size_t length,
                     size_t position, bool) override {
        if (length > static_cast<size_t>(INT_MAX) || position > static_cast<size_t>(INT_MAX))
            return -EOVERFLOW;
        int fd = decodeHandle(handle);
        if (::lseek(fd, static_cast<off_t>(position), SEEK_SET) < 0) return -EIO;
        ssize_t written = ::write(fd, buffer, length);
        return written == static_cast<ssize_t>(length) ? static_cast<int>(written)
               : written < 0 ? -errno : -EIO;
    }

    int preadHandle(uintptr_t handle, uint8_t *buffer, size_t length, size_t position) override {
        if (length > static_cast<size_t>(INT_MAX) || position > static_cast<size_t>(INT_MAX))
            return -EOVERFLOW;
        int fd = decodeHandle(handle);
        if (::lseek(fd, static_cast<off_t>(position), SEEK_SET) < 0) return -EIO;
        ssize_t got = ::read(fd, buffer, length);
        return got < 0 ? -errno : static_cast<int>(got);
    }

public:
    int remove(const char *path) override {
        cacheCloseAll();
        return ::unlink(fullPath(path).c_str()) < 0 ? -EIO : 0;
    }

    int rename(const char *oldPath, const char *newPath) override {
        cacheCloseAll();
        return ::rename(fullPath(oldPath).c_str(), fullPath(newPath).c_str()) < 0 ? -EIO : 0;
    }

    int truncate(const char *path, size_t length) override {
        cacheCloseAll();
        if (length > static_cast<size_t>(INT_MAX)) return -EOVERFLOW;
        return ::truncate(fullPath(path).c_str(), static_cast<off_t>(length)) < 0 ? -EIO : 0;
    }

};

// diskSpec "spiffs:<dir>": an SDFS image stitched from a directory's files.
class DiskImageStitchDir : private DirStorage, public DiskStitchImageSDFS {
public:
    explicit DiskImageStitchDir(const char *directory)
        : DirStorage(directory),
          DiskStitchImageSDFS(static_cast<DirStorage *>(this), 65535, "DSTITCH", false, true) {}

    void reportStats() override {
        DirStorage::printCacheStats();
    }
};

#ifdef CSIM
class DiskImageIMG : public DiskImage {
public:
    string filename;
    uint8_t *image = nullptr;
    size_t imageBytes = 0;

    explicit DiskImageIMG(const char *path) { open(path); }
    ~DiskImageIMG() override;
    void open(const char *path);
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
