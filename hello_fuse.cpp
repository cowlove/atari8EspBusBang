#define FUSE_USE_VERSION 30 // Specify FUSE API version
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>

uint8_t diskImg[] = {
#include "fuse_d2.h"	
};

static const char *filepath = "/fuse_d2.img";

class DiskImage { 
    public:
    size_t read(uint8_t *buf, size_t sector) {
        int offset = sector * sectorSize();
        for(int n = 0; n < sectorSize(); n++) { 
            buf[n] = diskImg[offset + n];
        }
        return sectorSize();
    };
    size_t write(const uint8_t *buf, size_t sector) { 
        int offset = sector * sectorSize();
        for(int n = 0; n < sectorSize(); n++) { 
            diskImg[offset + n] = buf[n];
        }
        return sectorSize();
    };
    int sectorSize() { return 128; } 
    int sectorCount() {  return 720; }
    bool valid() { return true; }
};

DiskImage disk;

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
        stbuf->st_size = sizeof(diskImg);
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
    printf("read(%d,%d)\n", (int)offset, (int)size);
    if (strcmp(path, filepath) == 0) {
        size_t len = sizeof(diskImg);
        if (offset < len) {
            if (offset + size > len) {
                size = len - offset;
            }
            for(int s = 0; s < size / disk.sectorSize(); s++) { 
                disk.read((uint8_t *)buf + s * disk.sectorSize(), s + offset / disk.sectorSize());
            }
        } else {
            size = 0;
        }
        return size;
    }
    return -ENOENT;
}

// Function to read file content
static int hello_write(const char *path, const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    (void) fi;
    printf("write(%d,%d)\n", (int)offset, (int)size);
    if (strcmp(path, filepath) == 0) {
        size_t len = sizeof(diskImg);
        if (offset < len) {
            if (offset + size > len) {
                size = len - offset;
            }
            for(int s = 0; s < size / disk.sectorSize(); s++) { 
                disk.write((const uint8_t *)buf + s * disk.sectorSize(), s + offset / disk.sectorSize());
            }
        } else {
            size = 0;
        }
        return size;
    }
    return -ENOENT;
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
    return fuse_main(argc, argv, &hello_oper, NULL);
}
