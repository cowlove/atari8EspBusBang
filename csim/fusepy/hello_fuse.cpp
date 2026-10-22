#define FUSE_USE_VERSION 30 // Specify FUSE API version
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>

#ifndef DRAM_ATTR
#define DRAM_ATTR
#define CSIM_DEFINED_DRAM_ATTR
#endif
inline void wifiRun() {}
#include "../../main/diskSmb.h"
#ifdef CSIM_DEFINED_DRAM_ATTR
#undef DRAM_ATTR
#undef CSIM_DEFINED_DRAM_ATTR
#endif
using std::string;
using std::vector;

static const char *filepath = "/fuse_d2.img";
static std::unique_ptr<DiskStitchGeneric<SmbConnection>> disk;

class LocalFS : public StorageInterface {
    int fd = -1;
    string prefix;
public:
    LocalFS(const char *p) : prefix(p) {}
    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>f) {
        LOG("readdir '%s'\n", path);
        string fn = prefix + "/" + path; 
        DIR *dir = opendir(fn.c_str());
        if (dir == NULL) {
            printf("opendir('%s') failed\n", fn.c_str());
            return 0;
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
        LOG("open('%s')\n", fn.c_str());
        return (fd = ::open(fn.c_str(), mode, perm)); 
    };
    int pwrite(const uint8_t *buf, size_t len, size_t pos) {
        LOG("pwrite(%d, %d)\n", (int)len, (int)pos);
        lseek(fd, pos, SEEK_SET);
        return write(fd, buf, len);
    } 
    int pread(uint8_t *buf, size_t len, size_t pos) {
        LOG("pread(%d, %d)\n", (int)len, (int)pos);
        lseek(fd, pos, SEEK_SET);
        return read(fd, buf, len);
    } 
    void close() { ::close(fd); fd = -1; };
};


const vector<ProcFsConnection::ProcFsFile> procFsNodes = 
{
    {   
        .name = "WIFI",
        .read =  [](string &buf) {},
        .write = [](const string &buf) {
            if (buf == "1") {
                //wifiRun();
            } else {
                // TODO figure out how to shutdown WIFI 
            }
        },
    },
    {   
        .name = "FOOP",
        .read =  [](string &buf) {
            buf = "foop contents\233";
            printf("read from FOOP\n");
        },
        .write = [](const string &buf) {
            printf("write to FOOP: '%s'\n", buf.c_str());
        },
    },
};

// Function to get file attributes (like size, permissions)
static int hello_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi; // Unused parameter
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) { // Root directory
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else if (strcmp(path, filepath) == 0) { // Our "hello" file
        stbuf->st_mode = S_IFREG | 0444; // Read-only file
        stbuf->st_nlink = 1;
        stbuf->st_size = disk->sectorSize() * (disk->sectorCount() - 3) + 3 * 128;
    } else {
        return -ENOENT; // File not found
    }
    return 0;
}

// Function to read directory contents
static int hello_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
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
    filler(buf, filepath + 1, NULL, 0, (fuse_fill_dir_flags)0); // Our "hello" file

    return 0;
}

// Function to read file content
static int hello_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    (void) fi;
    //printf("read(%d,%d)\n", (int)offset, (int)size);
    if (strcmp(path, filepath) != 0) 
        return -ENOENT;

    int bytesRead = 0;
    int bufOffset = offset - disk->sectorToOffset(disk->offsetToSector(offset));

    for(int s = disk->offsetToSector(offset); s <= disk->offsetToSector(offset + size); s++) {
        char buf2[512];  
        int r = disk->read((uint8_t *)buf2, disk->offsetToSector(offset + bytesRead));
        if (r < 0) 
            return r;
        int copyOutLen = std::min((int)size - bytesRead, disk->specificSectorSize(s) - bufOffset);
        //printf("read(%d,%d): r %d sector %d buffOffset %d copyOut %d bytesRead %d\n", 
        //    (int)offset, (int)size, r, s, bufOffset, copyOutLen, bytesRead);
        memcpy(buf + bytesRead, buf2 + bufOffset, copyOutLen);
        bytesRead += copyOutLen;
        bufOffset = 0;
    }
    return size;
}

// Function to read file content
static int hello_write(const char *path, const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    (void) fi;
    //printf("write(%d,%d)\n", (int)offset, (int)size);
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

static int hello_open(const char *path, 
                      struct fuse_file_info *fi) {
	fi->direct_io = 1;
	return 0;
}

// FUSE operations structure
static struct fuse_operations hello_oper = {
    .getattr = hello_getattr,
    .open = hello_open,
    .read = hello_read,
    .write = hello_write,
    .readdir = hello_readdir,
};

// Main function
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <smb-url> [FUSE options] <mountpoint>\n", argv[0]);
        fprintf(stderr, "Example: %s smb://localhost/pub -f ./mnt\n", argv[0]);
        return 2;
    }

    disk.reset(new DiskStitchGeneric<SmbConnection>(argv[1]));
    disk->start();

    vector<char *> fuseArgs;
    fuseArgs.push_back(argv[0]);
    for (int i = 2; i < argc; ++i)
        fuseArgs.push_back(argv[i]);

    return fuse_main(fuseArgs.size(), fuseArgs.data(), &hello_oper, NULL);
}
