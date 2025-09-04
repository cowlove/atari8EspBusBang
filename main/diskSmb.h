#pragma once 
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


#define SMB
#define SECTOR_SIZE 128

#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
using std::string;
using std::vector;

#include "diskImage.h"

inline void noprintf(const char *,...) {}
#define LOG noprintf

class StorageInterface {
public:
    virtual void connect() {};
    virtual int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>f) = 0;
    virtual int open(const char *path, int mode, int ac = 0) = 0;
    virtual int pwrite(const uint8_t *buf, size_t len, size_t pos) = 0; 
    virtual int pread(uint8_t *buf, size_t len, size_t pos) = 0; 
    virtual void close() = 0;
};


class SmbConnection : public StorageInterface {
    struct smb2_context *smb2 = NULL;
    struct smb2_url *url = NULL;
    struct smb2fh *fh = NULL;
    string lastFile;
    string password, urlString;
    public: 
    SmbConnection(const char *u, const char *pw = NULL) : urlString(u) {
        if (pw != NULL) password = pw;
    }
    ~SmbConnection() {
        //smb2_destroy_url(url);
        //smb2_destroy_context(smb2);
    }
    void connect() {
        smb2 = smb2_init_context();
        if (password != "")
            smb2_set_password(smb2, password.c_str());
        url = smb2_parse_url(smb2, urlString.c_str());
        if (smb2_connect_share(smb2, url->server, url->share, url->user) < 0) {
            printf("smb2_connect_share failed. %s\n", smb2_get_error(smb2));
        }
    }
    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>f) {
        LOG("readdir '%s'\n", path);
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
    string currentFile;
    int currentMode;
    int open(const char *path, int mode, int perm = 0) { 
        LOG("open('%s')\n", path);
        if (path == currentFile && mode == currentMode && fh != NULL)
            return 0;

        fh = smb2_open(smb2, path, mode);
        if (fh == NULL) { 
            printf("smb2_open failed. %s\n", smb2_get_error(smb2));
            return -1;
        }
        currentFile = path;
        currentMode = mode;
        return 0;
    }
    int pwrite(const uint8_t *buf, size_t len, size_t pos) { 
        LOG("pwrite(%d, %d)\n", (int)len, (int)pos);
        int count;
        int offset = 0;
        while (offset < len && (count = smb2_pwrite(smb2, fh, buf + offset, len - offset, pos + offset)) != 0) {
            LOG("smb2_pwrite(%d, %d) returned %d\n", (int)len - offset, (int)pos + offset, count);
            if (count == -EAGAIN) {
                continue;
            }
            if (count < 0) {
                printf("Failed to write file. %s\n", smb2_get_error(smb2));
                return count;
            }
            pos += count;
            offset += count;
        }
        LOG("pwrite(%d, %d) returning %d\n", (int)len, (int)pos, offset);
        return offset;
    }
    int pread(uint8_t *buf, size_t len, size_t pos) { 
        LOG("pread(%d, %d)\n", (int)len, (int)pos);
        int count;
        int offset = 0;
        while (offset < len && (count = smb2_pread(smb2, fh, buf + offset, len - offset, pos + offset)) != 0) {
            LOG("smb_pread(buf, %d, %d) returned %d\n", (int)len - offset, (int)pos + offset, count);
            if (count == -EAGAIN) {
                continue;
            }
            if (count < 0) {
                printf("Failed to read file. %s\n", smb2_get_error(smb2));
                return count;
            }
            //for(int n = 0; n < count; n++) putchar(buf[n]);
            //write(0, buf, count);
            pos += count;
            offset += count;
        }
        LOG("pread(%d, %d) returning %d\n", (int)len, (int)pos, offset);
        return offset;
    }
    void close() { 
        //if (fh != NULL) smb2_close(smb2, fh);
        //fh = NULL;
    }
};

//https://atari.fox-1.nl/disk-formats-explained/#atsd
struct __attribute__((packed)) Dos2Sector0 { 
    uint8_t unused = 0;
    uint8_t bootSectors = 3;
    uint16_t sectorSize = -1;
    uint16_t bootLoadAddr = 0;
    uint16_t bootInitAddr = 0;
    uint8_t bootJump = 0x45;
    uint16_t bootJumpAddr = 0;
    uint8_t maxOpenFiles = 3;
    uint8_t driveBits = 0x1;
};

struct __attribute__((packed)) Dos2VTOC { 
    uint8_t dosCode = 2;
    uint16_t usableSectors = -1;
    uint16_t freeSectors = -1;
    uint8_t unused[5] = {0};
    uint8_t bitmap[90] = {0xff};
    uint8_t unused2[28];

    static const int bpi = sizeof(bitmap[0]) * 8; // bits per index of bitmap 
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
    char filename[8] = {0};
    char extension[3] = {0};

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

struct Dos2DirectorySector_NO {
    Dos2Dirent entries[8];
};

struct StitchedFile {
    vector<uint16_t> sectors;
    string hostFile;
    int fileNo = -1;
    int flags = 0;
    size_t len = 0;
};

template <class IO>
class DiskStitchImage : public DiskImage {
    static const int numDirent = 64;
    Dos2VTOC vtoc;
    Dos2Sector0 sector1;
    //Dos2DirectorySector dirSectors[8];
    Dos2Dirent dirSectors[numDirent];
    //vector<StitchedFile> stitchedFiles; // TODO: currently assumed that stitchedFiles[n] correlates to dirSectors[n]
    StitchedFile stitchedFiles[numDirent];
    bool convertAtascii = false;
    string rootPath;
    int direntSecSize = 128; // only 128 bytes of directory sectors are used, even in DD
    IO io;

    bool stitchDir(const char *path) {
        rootPath = path;
        int filesStitched = 0;
        io.readdir(path, [this,path,&filesStitched](int n, dirent *entry, size_t len){
            LOG("readdir %s\n", entry->d_name);
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

    int findFreeDirent() { 
        for(int n = 0; n < numDirent; n++) { 
            if (dirSectors[n].flags == 0) return n;
        }
        return -1;
    }

    void charConvert(uint8_t *b, char from, char to) { 
        for(int n = 0; n < sectorDataSize(); n++) { 
            if (b[n] == from) b[n] = to;
        }
    }

    bool stitchFile(const char *fn, size_t fileLen) {
        LOG("stitchFile('%s', %d)\n", fn, (int)fileLen);
        StitchedFile newFile;
        newFile.hostFile = fn; //rootPath + "/" + fn;
        int sectorCount = (fileLen + sectorDataSize() - 1) / sectorDataSize();
        sectorCount = std::max(sectorCount, 1);
        int de = findFreeDirent();
        if (de < 0 || sectorCount > vtoc.freeSectors)
            return false;
        Dos2Dirent *dirent = &dirSectors[de];
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
        stitchedFiles[de] = newFile;
        return true;
    }

    StitchedFile *findFileBySector(int sector, int *offsetSector) { 
        for(int f = 0; f < numDirent; f++) {
            for(int i = 0; i < stitchedFiles[f].sectors.size(); i++) { 
                if ((stitchedFiles[f].flags & Dos2Dirent::inUse) != 0 && stitchedFiles[f].sectors[i] == sector) { 
                    *offsetSector = i;
                    return &stitchedFiles[f];
                }
            }
        }
        return NULL;
    }

public:
    DiskStitchImage(const char *url) : io(url) {
        for(int i = 0; i < sizeof(vtoc.bitmap)/sizeof(vtoc.bitmap[0]); i++) vtoc.bitmap[i] = -1;
        for(int i = 0; i <= 3; i++) vtoc.setBitmap(i, false);
        for(int i = 360; i <= 368; i++) vtoc.setBitmap(i, false);
        sector1.sectorSize = sectorSize();
        vtoc.freeSectors = vtoc.usableSectors = sectorCount() - 13;
    }

    void start() { 
        io.connect();
        stitchDir("");
    }

    int sectorDataSize() { return sectorSize() - 3; }
    int specificSectorSize(int s) { return (s <= 3) ? 128 : sectorSize(); }
    int offsetToSector(int off) { 
        return (off < 128 * 3) ? (off / 128) + 1 : (off - 3 * 128) / sectorSize() + 4;  
    } 
    int sectorToOffset(int s) { return s <= 3 ? (s - 1) * 128 : (s - 4) * sectorSize() + 3 * 128; }

    size_t read(uint8_t *buf, size_t sector) {
        //printf("sector rd %d buf %p\n", (int)sector, buf);
        if (sector == 1) memcpy(buf, &sector1, specificSectorSize(1));
        else if(sector == 360) {
            //printf("vtoc read\n");
            memcpy(buf, &vtoc, sizeof(vtoc));
        }
        else if(sector >= 361 && sector <= 368) {
            // first dirent in the sector we're reading
            int secStartDirent = (sector - 361) * direntSecSize / sizeof(dirSectors[0]); 
            //printf("dirent read - dirent %d\n", secStartDirent);
            memcpy(buf, &dirSectors[secStartDirent], direntSecSize); }
        else {
            int fsec;
            StitchedFile *sf = findFileBySector(sector, &fsec);
            if (sf != NULL) {
                // TODO: if this is last sector, stat the host file to see if it has grown and we should extend 
                // sf->sectors by one
                string fn = sf->hostFile;
                
                int r = io.open(fn.c_str(), O_RDONLY);
                if (r < 0) {
                    printf("error opening file '%s', errno=%d\n", fn.c_str(), errno);
                    return r;
                }
                int len = io.pread(buf, sectorDataSize(), fsec * sectorDataSize());
                if (convertAtascii) 
                    charConvert(buf, '\n', '\233');
                io.close();
                if (len < 0) {
                    printf("error reading file '%s'\n", fn.c_str());
                    return len;
                }
                //printf("reading sector %d for file '%s' fsec %d, len %d\n", (int)sector, fn.c_str(), fsec, len); 
                int nextSector = sf->sectors.size() > fsec + 1 ? (sf->sectors[fsec + 1]) : 0;
                //printf("File '%s' fileno %d, sec offset %d, next sec %d\n", fn.c_str(), sf->fileNo, fsec, nextSector);
                int tailBytes = specificSectorSize(sector) - 3;
                buf[tailBytes + 0] = (sf->fileNo << 2) | ((nextSector & 0x300) >> 8);
                buf[tailBytes + 1] = nextSector & 0xff;
                buf[tailBytes + 2] = len;
            } else { 
                bzero(buf, sectorSize());
            }
        }
        return specificSectorSize(sector);
    };
    size_t write(const uint8_t *buf, size_t sector) {
        //printf("sector wr %d\n", (int)sector);
        if(sector == 360) {
            printf("Writing VTOC.  Before write:\n");
            vtoc.printBitmap();
            memcpy(&vtoc, buf, sizeof(vtoc));
            printf("Writing VTOC.  After write:\n");
            vtoc.printBitmap();
        } else if(sector >= 361 && sector <= 368) {
            // first dirent in the sector we're reading
            int secStartDirent = (sector - 361) * direntSecSize / sizeof(dirSectors[0]); 
            memcpy(&dirSectors[secStartDirent], buf, direntSecSize); //sectorSize());
            for(int n = 0; n < numDirent; n++) {
                Dos2Dirent *d = &dirSectors[n];
                StitchedFile *sf = &stitchedFiles[n];
                if (1 && d->flags != 0) {
                    printf("dirent%02d: ", n);
                    d->print();
                }
                if (d->flags == stitchedFiles[n].flags) 
                    continue;

                printf("dirent flags changed %02x!=%02x:", d->flags, stitchedFiles[n].flags);
                d->print();
                // TODO: file truncated by writing flags to 0x43 and d->count to 1, we should trim
                //   sf->sectors[], look into removing the field sf->len, and call truncate(2)
                // TODO: file erased by writing flags to 0x80, we should call rm(2) 
                // TODO: here is where we could delete a file or notice a new file created
                // TODO: change stitchedFiles from a vector to an array sized to match the disk dir table
                if (d->flags != 0 && sf->flags == 0) { 
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
                    newFile.hostFile = filename;
                    printf("creating '%s'\n", newFile.hostFile.c_str());
                    int fd = io.open(newFile.hostFile.c_str(), O_CREAT | O_RDWR, 0644);
                    if (fd < 0) {
                        printf("error creating '%s'\n", newFile.hostFile.c_str());
                    } else { 
                        io.close();
                    }
                    *sf = newFile;
                }
             } 
        } else {
            int tailBytes = sectorSize() - 3;
            int fileNo = buf[tailBytes] >> 2;
            int nextSector = ((buf[tailBytes] & 0x3) << 8) + buf[tailBytes + 1];
            int len = buf[tailBytes + 2];
            LOG("write for fileno %d, len %d, next sector %d\n", fileNo, len, nextSector);
            int fsec;
            StitchedFile *sf = findFileBySector(sector, &fsec);
            if (sf != NULL) { 
                // TODO: if nextSector == 0 we should trip sf->sectors[] and call truncate(2)

                string &fn = sf->hostFile;
                
                int r = io.open(fn.c_str(), O_RDWR);
                if (r < 0) {
                    printf("error opening file '%s', errno=%d\n", fn.c_str(), errno);
                    return r;
                }
                if (convertAtascii) { 
                    static uint8_t buf2[512]; // TODO
                    memcpy(buf2, buf, sectorSize());
                    charConvert(buf2, '\233', '\n');
                    r = io.pwrite(buf2, len, fsec * sectorDataSize());
                } else { 
                    r = io.pwrite(buf, len, fsec * sectorDataSize());
                }
                io.close();
                if (r < 0) {
                    printf("error writing file '%s'\n", fn.c_str());
                    return r;
                }
                // Writing past end of file, extend sf->sectors
                if (nextSector != 0 && sf->sectors.size() == fsec + 1) 
                    sf->sectors.push_back(nextSector);
            }
        }
        return sectorSize();
    };
    int sectorSize() { return SECTOR_SIZE; } //XXSECSIZE  
    int sectorCount() { return 720; }
    bool valid() { return true; }
};
