#define FUSE_USE_VERSION 30 // Specify FUSE API version
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>
#include <string>
#include <vector>
#include <functional>
uint8_t diskImg[] = {
#include "fuse_d2.h"	
};

#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
void smbReq();
using std::string;
using std::vector;

static const char *filepath = "/fuse_d2.img";




class StorageInterface {
public:
    virtual void connect() {};
    virtual int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>f) = 0;
    virtual int open(const char *path, int mode, int ac = 0) = 0;
    virtual int pwrite(const uint8_t *buf, size_t len, size_t pos) = 0; 
    virtual int pread(uint8_t *buf, size_t len, size_t pos) = 0; 
    virtual void close() = 0;
};

class LocalFS : public StorageInterface {
    int fd = -1;
    string prefix;
public:
    LocalFS(const char *p) : prefix(p) {}
    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>f) {
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
        return (fd = ::open(fn.c_str(), mode, perm)); 
    };
    int pwrite(const uint8_t *buf, size_t len, size_t pos) {
        lseek(fd, pos, SEEK_SET);
        return write(fd, buf, len);
    } 
    int pread(uint8_t *buf, size_t len, size_t pos) {
        lseek(fd, pos, SEEK_SET);
        return read(fd, buf, len);
    } 
    void close() { ::close(fd); fd = -1; };
};

class SmbConnection : public StorageInterface {
    struct smb2_context *smb2;
    struct smb2_url *url;
    struct smb2fh *fh = NULL;
    string lastFile;
    public: 
    SmbConnection(const char *u, const char *password = NULL) {
        smb2 = smb2_init_context();
        if (password != NULL)
            smb2_set_password(smb2, password);
        url = smb2_parse_url(smb2, u);
    }
    ~SmbConnection() {
        smb2_destroy_url(url);
        smb2_destroy_context(smb2);
    }
    void connect() {
        if (smb2_connect_share(smb2, url->server, url->share, url->user) < 0) {
            printf("smb2_connect_share failed. %s\n", smb2_get_error(smb2));
        }
    }
    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>f) {
        struct smb2dir *dir;
        struct smb2dirent *ent;
        dir = smb2_opendir(smb2, path);
        if (dir == NULL) { 
            printf("readdir failed. %s\n", smb2_get_error(smb2));
            return 0;
        }
        int count = 0;
        while ((ent = smb2_readdir(smb2, dir))) {
            if (ent->st.smb2_type != SMB2_TYPE_FILE)
                continue;
            struct dirent de = {0};
            strncpy(de.d_name, ent->name, sizeof(de.d_name));
            de.d_type = DT_REG;
            f(count, &de, ent->st.smb2_size);
            count++;
        }
        smb2_closedir(smb2, dir);
        return count;
    }
    int open(const char *path, int mode, int perm = 0) { 
        fh = smb2_open(smb2, path, mode);
        if (fh == NULL) { 
            printf("smb2_open failed. %s\n", smb2_get_error(smb2));
            return -1;
        }
        return 0;
    }
    int pwrite(const uint8_t *buf, size_t len, size_t pos) { 
        int count;
        while ((count = smb2_pwrite(smb2, fh, buf, sizeof(buf), pos)) != 0) {
            if (count == -EAGAIN) {
                continue;
            }
            if (count < 0) {
                printf("Failed to write file. %s\n", smb2_get_error(smb2));
                break;
            }
            pos += count;
        }
        return pos;
    }
    int pread(uint8_t *buf, size_t len, size_t pos) { 
        int count;
        while ((count = smb2_pread(smb2, fh, buf, sizeof(buf), pos)) != 0) {
            if (count == -EAGAIN) {
                continue;
            }
            if (count < 0) {
                printf("Failed to read file. %s\n", smb2_get_error(smb2));
                break;
            }
            for(int n = 0; n < count; n++) putchar(buf[n]);
            //write(0, buf, count);
            pos += count;
        }
        return pos;
    }
    void close() { 
        if (fh != NULL) smb2_close(smb2, fh);
        fh = NULL;
    }
};

//https://atari.fox-1.nl/disk-formats-explained/#atsd
struct __attribute__((packed)) Dos2Sector0 { 
    uint8_t unused = 0;
    uint8_t bootSectors = 3;
    uint16_t bootLoadAddr = 0;
    uint16_t bootInitAddr = 0;
    uint8_t bootJump = 0x45;
    uint16_t bootJumpAddr = 0;
    uint8_t maxOpenFiles = 3;
    uint8_t driveBits = 0x1;
};

struct __attribute__((packed)) Dos2VTOC { 
    uint8_t dosCode = 2;
    uint16_t usableSectors = 707;
    uint16_t freeSectors = 707;
    uint8_t unused[5] = {0};
    uint8_t bitmap[90] = {0xff};
    uint8_t unused2[28];

    static const int bpi = sizeof(bitmap[0]) * 8; // bits per index of bitmap 
    Dos2VTOC() { 
        for(int i = 0; i < sizeof(bitmap)/sizeof(bitmap[0]); i++) bitmap[i] = -1;
        for(int i = 0; i <= 3; i++) setBitmap(i, false);
        for(int i = 360; i <= 368; i++) setBitmap(i, false);
    }
    // bitmap is high bit first within the byte 
    void setBitmap(int sector, bool value) {
        if (value) 
            bitmap[sector / bpi] |= (1 << (bpi - 1 - (sector & (bpi - 1))));
        else
            bitmap[sector / bpi] &= ~(1 << (bpi - 1 - (sector & (bpi - 1))));
    }    
    bool getBitmap(int sector) { 
        return (bitmap[sector / bpi] & (1 << (bpi - 1 - (sector & (bpi - 1))))) != 0; 
    }
    int findFreeSector() { 
        for(int n = 0; n < sizeof(bitmap)/sizeof(bitmap[0]); n++) {
            for(int b = 0; b < bpi; b++) { 
                if ((bitmap[n] & (1 << (bpi - 1 - b))) != 0)
                    return n * bpi + b;
            }
        }
        return -1;
    }
    void printBitmap() { 
        printf("VTOC:\n");
        const int linebreak = 94;
        for(int n = 0; n < 720; n++) {
            if (n % linebreak == 0) printf("%03d: ", n); 
            printf("%d", getBitmap(n));
            if (n % linebreak == linebreak - 1) printf("\n");
        }
        printf("\n");
    }  
};

struct __attribute__((packed)) Dos2Dirent { 
    uint8_t flags = 0;
    uint16_t sectorCount = 0;
    uint16_t startSector = 0;
    char filename[8] = {' '};
    char extension[3] = {' '};

    static const int openForOutput = 0x1;
    static const int dos2 = 0x2;
    static const int locked = 0x20;
    static const int inUse = 0x40;
    static const int deleted = 0x80;

    void setFilename(const char *fn) { 
        for(int n = 0; n < std::min(strlen(fn), sizeof(filename)); n++) filename[n] = toupper(fn[n]);
        for(int n = strlen(fn); n < sizeof(filename); n++) filename[n] = ' ';
        for(int n = 0; n < sizeof(extension); n++) extension[n] = ' ';
    }
    string getFilename() { 
        string fn;
        for(int n = 0; n < sizeof(filename); n++) {
            if (filename[n] != ' ') fn += filename[n];
        }
        return fn;
    }
    void print() {
        string name(filename, filename + sizeof(filename)), ext(extension, extension + sizeof(extension)); 
        printf("'%s%s' start=%03d count=%03d flags=0x%02x\n", name.c_str(), ext.c_str(), startSector, sectorCount, flags);
    }
};

struct Dos2DirectorySector {
    Dos2Dirent entries[8];
};

struct StitchedFile {
    vector<uint16_t> sectors;
    string hostFile;
    int fileNo = -1;
    int flags = 0;
    size_t len = 0;
};

class DiskFictImage {
public:
    Dos2VTOC vtoc;
    Dos2Sector0 sector1;
    Dos2DirectorySector dirSectors[8];
    uint8_t dataSector[128] = {0};
    vector<StitchedFile> stitchedFiles; // TODO: currently assumed that stitchedFiles[n] correlates to dirSectors[0].entries[n]
    bool convertAtascii = false;
    string rootPath;
#if 1
    LocalFS io = LocalFS("/home/jim/src/fusepy");
#else
    SmbConnection io = SmbConnection("smb://jim@localhost/pub/");
#endif
    bool stitchDir(const char *path) {
        rootPath = path;
        int filesStitched = 0;
        io.readdir(path, [this,path,&filesStitched](int n, dirent *entry, size_t len){
            printf("readdir %s\n", entry->d_name);
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                return;
            string fn = string(rootPath) + "/" + path + "/" + entry->d_name;
            if (len > vtoc.freeSectors * sectorDataSize())
                return;
            stitchFile(entry->d_name, len);
                filesStitched++;
        }); 
        return filesStitched > 0;
    }

    DiskFictImage() {
        io.connect();
        stitchDir("hostdir");
    }

    int findFreeDirent() { 
        for(int n = 0; n < sizeof(dirSectors) / sizeof(Dos2Dirent); n++) { 
            if (dirSectors[0].entries[n].flags == 0) return n;
        }
        return -1;
    }

    void charConvert(uint8_t *b, char from, char to) { 
        for(int n = 0; n < sectorDataSize(); n++) { 
            if (b[n] == from) b[n] = to;
        }
    }

    bool stitchFile(const char *fn, size_t fileLen) {
        StitchedFile newFile;
        newFile.hostFile = rootPath + "/" + fn;
        int sectorCount = (fileLen + sectorDataSize() - 1) / sectorDataSize();
        sectorCount = std::max(sectorCount, 1);
        int de = findFreeDirent();
        if (de < 0 || sectorCount > vtoc.freeSectors)
            return false;
        Dos2Dirent *dirent = &dirSectors[0].entries[de];
        newFile.fileNo = de;
        newFile.len = fileLen;
        newFile.flags = Dos2Dirent::dos2 | Dos2Dirent::inUse;
        //printf("allocting for file '%s', fileLen %d sectorCount %d\n", fn, fileLen, sectorCount);
        for(int i = 0; i < sectorCount; i++) { 
            int s = vtoc.findFreeSector();
            //printf("allocating sector %d for file '%s'\n", s, fn);
            if (s < 0) {
                for(auto s2: newFile.sectors) vtoc.setBitmap(s2, true);
                return false;
            }
            newFile.sectors.push_back(s);
            vtoc.setBitmap(s, false);
        }
        dirent->flags = newFile.flags;
        const char *basename = strrchr((const char *)newFile.hostFile.c_str(), '/');
        if (basename == NULL) {
            basename = newFile.hostFile.c_str();
        } else { 
            basename++;
        }
        dirent->setFilename(basename);
        dirent->startSector = newFile.sectors.size() > 0 ? newFile.sectors[0] : 0;
        dirent->sectorCount = sectorCount;
        vtoc.freeSectors -= sectorCount;
        stitchedFiles.push_back(newFile);
        return true;
    }

    StitchedFile *findFileBySector(int sector, int *offsetSector) { 
        for(int f = 0; f < stitchedFiles.size(); f++) {
            for(int i = 0; i < stitchedFiles[f].sectors.size(); i++) { 
                if (stitchedFiles[f].sectors[i] == sector) { 
                    *offsetSector = i;
                    return &stitchedFiles[f];
                }
            }
        }
        return NULL;
    }
    size_t read(uint8_t *buf, size_t sector) {
        //printf("sector rd %d\n", (int)sector);
        if (sector == 1) memcpy(buf, &sector1, sectorSize());
        else if(sector == 360) memcpy(buf, &vtoc, sectorSize());
        else if(sector >= 361 && sector <= 368) { memcpy(buf, &dirSectors[sector - 361], sectorSize()); }
        else {
            int fsec;
            StitchedFile *sf = findFileBySector(sector, &fsec);
            if (sf != NULL) {
                // TODO: if this is last sector, stat the host file to see if it has grown and we should extend 
                // sf->sectors by one
                string fn = sf->hostFile;
                
                if (io.open(fn.c_str(), O_RDONLY) < 0) {
                    printf("error opening file '%s', errno=%d\n", fn.c_str(), errno);
                    return -1;
                }
                int len = io.pread(buf, sectorDataSize(), fsec * sectorDataSize());
                if (convertAtascii) 
                    charConvert(buf, '\n', '\233');
                io.close();
                if (len < 0) {
                    printf("error reading file '%s'\n", fn.c_str());
                    return -1;
                }
                //printf("reading sector %d for file '%s' fsec %d, len %d\n", (int)sector, fn.c_str(), fsec, len); 
                int nextSector = sf->sectors.size() > fsec + 1 ? (sf->sectors[fsec + 1]) : 0;
                printf("File '%s' fileno %d, sec offset %d, next sec %d\n", fn.c_str(), sf->fileNo, fsec, nextSector);
                buf[125] = (sf->fileNo << 2) | ((nextSector & 0x300) >> 8);
                buf[126] = nextSector & 0xff;
                buf[127] = len;
            } else { 
                bzero(buf, sectorSize());
            }
        }
        return sectorSize();
    };
    size_t write(const uint8_t *buf, size_t sector) {
        printf("sector wr %d\n", (int)sector);
        if(sector == 360) {
            printf("Writing VTOC.  Before write:\n");
            vtoc.printBitmap();
            memcpy(&vtoc, buf, sectorSize());
            printf("Writing VTOC.  After write:\n");
            vtoc.printBitmap();
        } else if(sector >= 361 && sector <= 368) {
             memcpy(&dirSectors[sector - 361], buf, sectorSize());
             for(int n = 0; n < sizeof(dirSectors)/sizeof(Dos2Dirent); n++) {
                Dos2Dirent *d = &dirSectors[0].entries[n];
                if (1 && d->flags != 0) {
                    printf("dirent%02d: ", n);
                    d->print();
                }
                if (n < stitchedFiles.size() && d->flags != stitchedFiles[n].flags) { 
                    printf("dirent flags changed %02x!=%02x:", d->flags, stitchedFiles[n].flags);
                    d->print();
                    // TODO: file truncated by writing flags to 0x43 and d->count to 1, we should trim
                    //   sf->sectors[], look into removing the field sf->len, and call truncate(2)
                    // TODO: file erased by writing flags to 0x80, we should call rm(2) 
                    // TODO: here is where we could delete a file or notice a new file created
                    // TODO: change stitchedFiles from a vector to an array sized to match the disk dir table
                }
                if (d->flags != 0 && n >= stitchedFiles.size()) { 
                    printf("new dirent: ");
                    d->print();
                    StitchedFile newFile;
                    newFile.fileNo = n;
                    newFile.flags = d->flags;
                    newFile.sectors.push_back(d->startSector);
                    string filename;
                    for(int n = 0; n < sizeof(d->filename); n++) {
                        if (d->filename[n] != ' ') filename += d->filename[n];
                    }
                    newFile.hostFile = rootPath + "/" + filename;
                    printf("creating '%s'\n", newFile.hostFile.c_str());
                    int fd = io.open(newFile.hostFile.c_str(), O_CREAT, 0644);
                    if (fd < 0) {
                        printf("error creating '%s'\n", newFile.hostFile.c_str());
                    } else { 
                        io.close();
                    }
                    stitchedFiles.push_back(newFile);
                }
             } 
        } else {
            int fileNo = buf[125] >> 2;
            int nextSector = ((buf[125] & 0x3) << 8) + buf[126];
            int len = buf[127];
            printf("write for fileno %d, len %d, next sector %d\n", fileNo, len, nextSector);
            int fsec;
            StitchedFile *sf = findFileBySector(sector, &fsec);
            if (sf != NULL) { 
                // TODO: if nextSector == 0 we should trip sf->sectors[] and call truncate(2)

                string &fn = sf->hostFile;
                
                if (io.open(fn.c_str(), O_RDWR)) {
                    printf("error opening file '%s', errno=%d\n", fn.c_str(), errno);
                    return -1;
                }
                printf("write::open() returned\n");
                int r;
                if (convertAtascii) { 
                    static uint8_t buf2[128]; // TODO
                    memcpy(buf2, buf, sizeof(buf2));
                    charConvert(buf2, '\233', '\n');
                    r = io.pwrite(buf2, len, fsec * sectorDataSize());
                } else { 
                    r = io.pwrite(buf, len, fsec * sectorDataSize());
                }
                io.close();
                if (r != len) {
                    printf("error writing file '%s'\n", fn.c_str());
                    return -1;
                }
                // Writing past end of file, extend sf->sectors
                if (nextSector != 0 && sf->sectors.size() == fsec + 1) 
                    sf->sectors.push_back(nextSector);
            }
        }
        return sectorSize();
    };
    int sectorDataSize() { return 125; }
    int sectorSize() { return 128; } 
    int sectorCount() {  return 720; }
    bool valid() { return true; }
};

DiskFictImage disk;

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
        stbuf->st_size = disk.sectorSize() * disk.sectorCount();
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
    if (strcmp(path, filepath) == 0) {
        size_t len = sizeof(diskImg);
        if (offset < len) {
            if (offset + size > len) {
                size = len - offset;
            }
            for(int s = 0; s < size / disk.sectorSize(); s++) { 
                disk.read((uint8_t *)buf + s * disk.sectorSize(), s + offset / disk.sectorSize() + 1);
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
    //printf("write(%d,%d)\n", (int)offset, (int)size);
    if (strcmp(path, filepath) == 0) {
        size_t len = sizeof(diskImg);
        if (offset < len) {
            if (offset + size > len) {
                size = len - offset;
            }
            for(int s = 0; s < size / disk.sectorSize(); s++) { 
                disk.write((const uint8_t *)buf + s * disk.sectorSize(), s + offset / disk.sectorSize() + 1);
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
    SmbConnection smb("smb://jim@localhost/pub/hostdir");
    smb.connect();
    uint8_t buf[1024];
    smb.open("foo", O_RDONLY);
    int n = smb.pread(buf, sizeof(buf), 0);
    smb.close();
    smb.readdir("foodir", [](int n, struct dirent *d, size_t len) { 
        printf("dirent %d: %s len %d\n", n, d->d_name, (int)len);
    });
    printf("smb.read() returned %d\n", n);
    disk.vtoc.printBitmap();
    return fuse_main(argc, argv, &hello_oper, NULL);
}
