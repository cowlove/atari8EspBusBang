#define FUSE_USE_VERSION 30 // Specify FUSE API version
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <inttypes.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <errno.h>
#include <stdlib.h>

#ifndef DRAM_ATTR
#define DRAM_ATTR
#define CSIM_DEFINED_DRAM_ATTR
#endif
inline void wifiRun() {}
#include "../../main/diskStitch.h"
#include "../../main/diskStitchImageSDFS.h"
#include "../../main/diskStitchSmb.h"
#include "../../main/diskStitchProc.h"
#ifdef CSIM_DEFINED_DRAM_ATTR
#undef DRAM_ATTR
#undef CSIM_DEFINED_DRAM_ATTR
#endif
using std::string;
using std::vector;

static const char *filepath = "/fuse_d2.img";
static std::unique_ptr<DiskImage> disk;

class LocalFS : public StorageInterface {
    int fd = -1;
    string prefix;
public:
    LocalFS(const char *p) : prefix(p) {
        char absolutePath[PATH_MAX];
        if (realpath(p, absolutePath))
            prefix = absolutePath;
        LOG("LocalFS path=%s resolved=%s\n", p, prefix.c_str());
    }
    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>f) {
        LOG("readdir '%s'\n", path);
        string fn = prefix + "/" + path; 
        DIR *dir = opendir(fn.c_str());
        if (dir == NULL) {
            printf("opendir('%s') failed\n", fn.c_str());
            return -errno;
        }

        int count = 0;
        dirent *entry;
        while ((entry = ::readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            fn = prefix + "/" + path + "/" + entry->d_name;
            struct stat st; 
            if (stat(fn.c_str(), &st)) 
                continue;
            if (S_ISREG(st.st_mode) == false)
                continue;
            f(count, entry, st.st_size);
            count++;
        }
        closedir(dir);
        return count;
    }
    int open(const char *path, int mode, int perm = 0) {
        string fn = prefix + "/" + path; 
        fd = ::open(fn.c_str(), mode, perm);
        LOG("open('%s') mode=%x fd=%d\n", fn.c_str(), mode, fd);
        return fd;
    };
    int pwrite(const uint8_t *buf, size_t len, size_t pos, bool) {
        LOG("pwrite(%d, %d)\n", (int)len, (int)pos);
        if (lseek(fd, pos, SEEK_SET) < 0) return -errno;
        return write(fd, buf, len);
    } 
    int pread(uint8_t *buf, size_t len, size_t pos) {
        LOG("pread(%d, %d)\n", (int)len, (int)pos);
        if (lseek(fd, pos, SEEK_SET) < 0) return -errno;
        int count = read(fd, buf, len);
        LOG("pread result=%d\n", count);
        return count;
    } 
    int remove(const char *path) override {
        string fn = prefix + "/" + path;
        return unlink(fn.c_str()) < 0 ? -errno : 0;
    }
    int truncate(const char *path, size_t len) override {
        string fn = prefix + "/" + path;
        return ::truncate(fn.c_str(), len) < 0 ? -errno : 0;
    }
    void close() { ::close(fd); fd = -1; };
};

static std::unique_ptr<LocalFS> localStorage;
static std::unique_ptr<SmbConnection> smbStorage;

// Function to get file attributes (like size, permissions)
static int dstitch_mount_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi; // Unused parameter
    LOG("FUSE getattr path=%s\n", path);
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) { // Root directory
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else if (strcmp(path, filepath) == 0) { // The virtual disk image
        stbuf->st_mode = S_IFREG | 0444; // Read-only file
        stbuf->st_nlink = 1;
        stbuf->st_size = disk->imageSize();
        LOG("FUSE getattr image mode=%o size=%" PRIdMAX "\n",
            stbuf->st_mode, static_cast<intmax_t>(stbuf->st_size));
    } else {
        return -ENOENT; // File not found
    }
    return 0;
}

// Function to read directory contents
static int dstitch_mount_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags) {
    (void) offset;
    (void) fi;
    (void) flags;

    if (strcmp(path, "/") != 0) {
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0, (fuse_fill_dir_flags)0);  // Current directory
    filler(buf, "..", NULL, 0, (fuse_fill_dir_flags)0); // Parent directory
    filler(buf, filepath + 1, NULL, 0, (fuse_fill_dir_flags)0); // The virtual disk image

    return 0;
}

// Function to read file content
static int dstitch_mount_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    (void) fi;
    //printf("read(%d,%d)\n", (int)offset, (int)size);
    if (strcmp(path, filepath) != 0) 
        return -ENOENT;

    LOG("FUSE read offset=%" PRIdMAX " size=%zu\n", static_cast<intmax_t>(offset), size);
    if (offset < 0 || static_cast<size_t>(offset) >= disk->imageSize()) return 0;
    size = std::min(size, disk->imageSize() - static_cast<size_t>(offset));
    size_t bytesRead = 0;
    while (bytesRead < size) {
        size_t currentOffset = static_cast<size_t>(offset) + bytesRead;
        int sector = disk->offsetToSector(currentOffset);
        size_t sectorOffset = currentOffset - disk->sectorToOffset(sector);
        uint8_t sectorBuffer[512];
        size_t result = disk->read(sectorBuffer, sector);
        LOG("FUSE read sector=%d sectorOffset=%zu\n", sector, sectorOffset);
        if (static_cast<int>(result) < 0) return static_cast<int>(result);
        size_t chunk = std::min(size - bytesRead,
                                static_cast<size_t>(disk->specificSectorSize(sector)) - sectorOffset);
        memcpy(buf + bytesRead, sectorBuffer + sectorOffset, chunk);
        bytesRead += chunk;
    }
    return static_cast<int>(bytesRead);
}

// Function to read file content
static int dstitch_mount_write(const char *path, const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    (void) fi;
    LOG("FUSE write path=%s offset=%" PRIdMAX " size=%zu\n",
        path, static_cast<intmax_t>(offset), size);
    if (strcmp(path, filepath) != 0)
        return -ENOENT;
    size_t bytesWritten = 0;
    while (bytesWritten < size) {
        off_t currentOffset = offset + bytesWritten;
        int sector = disk->offsetToSector(currentOffset);
        size_t sectorOffset = currentOffset - disk->sectorToOffset(sector);
        size_t sectorSize = disk->specificSectorSize(sector);
        size_t chunkSize = std::min(size - bytesWritten, sectorSize - sectorOffset);
        int r;

        if (sectorOffset == 0 && chunkSize == sectorSize) {
            r = disk->write((const uint8_t *)buf + bytesWritten, sector);
        } else {
            uint8_t sectorBuffer[512];
            r = disk->read(sectorBuffer, sector);
            if (r < 0)
                return r;
            memcpy(sectorBuffer + sectorOffset, buf + bytesWritten, chunkSize);
            r = disk->write(sectorBuffer, sector);
        }
        if (r < 0)
            return r;

        LOG("write(%d,%d): r %d sector %d sectorOffset %d chunk %d bytesWritten %d\n",
            (int)offset, (int)size, r, sector, (int)sectorOffset,
            (int)chunkSize, (int)bytesWritten);
        bytesWritten += chunkSize;
    }
    return size;
}

static int dstitch_mount_open(const char *path, 
                      struct fuse_file_info *fi) {
	LOG("FUSE open path=%s flags=%x\n", path, fi->flags);
	fi->direct_io = 1;
	return 0;
}

// FUSE operations structure
static struct fuse_operations dstitch_mount_oper = {
    .getattr = dstitch_mount_getattr,
    .open = dstitch_mount_open,
    .read = dstitch_mount_read,
    .write = dstitch_mount_write,
    .readdir = dstitch_mount_readdir,
};

// Main function
int main(int argc, char *argv[]) {
    enum class ImageFormat { Dos20, Sdfs };
    ImageFormat format = ImageFormat::Dos20;
    bool explicitFormat = false;
    bool explicitSdfsSectorCount = false;
    int sdfsSectorCount = 65535;
    const char *resource = NULL;
    vector<char *> fuseArgs;
    fuseArgs.push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dos20") == 0 || strcmp(argv[i], "--sdfs") == 0) {
            if (explicitFormat) {
                fprintf(stderr, "Specify only one of --dos20 or --sdfs\n");
                return 2;
            }
            explicitFormat = true;
            format = strcmp(argv[i], "--sdfs") == 0 ? ImageFormat::Sdfs : ImageFormat::Dos20;
        } else if (strcmp(argv[i], "--sdfs-sectors") == 0 ||
                   strncmp(argv[i], "--sdfs-sectors=", 15) == 0) {
            const char *value;
            if (argv[i][14] == '=') {
                value = argv[i] + 15;
            } else {
                if (++i >= argc) {
                    fprintf(stderr, "--sdfs-sectors requires a sector count\n");
                    return 2;
                }
                value = argv[i];
            }
            char *end = NULL;
            errno = 0;
            long parsed = strtol(value, &end, 10);
            if (errno || end == value || *end || parsed < 64 || parsed > 65535) {
                fprintf(stderr, "SDFS sector count must be between 64 and 65535\n");
                return 2;
            }
            explicitSdfsSectorCount = true;
            sdfsSectorCount = static_cast<int>(parsed);
        } else if (strncmp(argv[i], "file://", 7) == 0 ||
                   strncmp(argv[i], "smb://", 6) == 0 ||
                   strncmp(argv[i], "proc:", 5) == 0) {
            if (resource) {
                fprintf(stderr, "Specify exactly one backing resource\n");
                return 2;
            }
            resource = argv[i];
        } else {
            fuseArgs.push_back(argv[i]);
        }
    }

    if (!resource || fuseArgs.size() < 2) {
        fprintf(stderr, "Usage: %s [--dos20|--sdfs [--sdfs-sectors N]] <file://directory|smb://url|proc:> [FUSE options] <mountpoint>\n", argv[0]);
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s file://./hostdir -f ./mnt                 # DOS 2.0 (default)\n", argv[0]);
        fprintf(stderr, "  %s --sdfs file://./hostdir -f ./mnt          # SDFS\n", argv[0]);
        fprintf(stderr, "  %s smb://localhost/pub -f ./mnt              # DOS 2.0 over SMB\n", argv[0]);
        fprintf(stderr, "  %s proc: -f ./mnt                           # ProcFS image\n", argv[0]);
        return 2;
    }

    if (explicitSdfsSectorCount && format != ImageFormat::Sdfs) {
        fprintf(stderr, "--sdfs-sectors requires --sdfs\n");
        return 2;
    }

    if (strncmp(resource, "proc:", 5) == 0) {
        if (explicitFormat) {
            fprintf(stderr, "Image format flags cannot be used with proc:\n");
            return 2;
        }
        disk.reset(new DiskProcFs(procFsNodes));
    } else {
        StorageInterface *storage = NULL;
        if (strncmp(resource, "file://", 7) == 0) {
            if (!resource[7]) {
                fprintf(stderr, "The file:// resource requires a backing directory\n");
                return 2;
            }
            localStorage.reset(new LocalFS(resource + 7));
            storage = localStorage.get();
        } else if (strncmp(resource, "smb://", 6) == 0) {
            smbStorage.reset(new SmbConnection(resource));
            storage = smbStorage.get();
        }

        if (!storage) {
            fprintf(stderr, "Unknown backing resource '%s'; expected 'file://', 'smb://' or 'proc:'\n", resource);
            return 2;
        }

        if (format == ImageFormat::Sdfs) {
            DiskStitchImageSDFS *sdfsDisk = new DiskStitchImageSDFS(storage, sdfsSectorCount);
            if (!sdfsDisk->valid()) {
                fprintf(stderr, "Failed to synthesize SDFS image from '%s'\n", resource);
                delete sdfsDisk;
                return 1;
            }
            disk.reset(sdfsDisk);
        } else {
            DiskStitchImage *dos20Disk = new DiskStitchImage(storage);
            dos20Disk->start();
            disk.reset(dos20Disk);
        }
    }

    return fuse_main(fuseArgs.size(), fuseArgs.data(), &dstitch_mount_oper, NULL);
}
