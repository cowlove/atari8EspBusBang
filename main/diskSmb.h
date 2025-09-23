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
#ifndef SECTOR_SIZE
#define SECTOR_SIZE 256
#endif

#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
using std::string;
using std::vector;

#include "diskImage.h"
#include "util.h"

inline void noprintf(const char *,...) {}
#ifndef LOG
#define LOG noprintf
#endif

class StorageInterface {
public:
    virtual void connect() {};
    virtual int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>f) = 0;
    virtual int open(const char *path, int mode, int ac = 0) = 0;
    virtual int pwrite(const uint8_t *buf, size_t len, size_t pos, bool eof) = 0; 
    virtual int pread(uint8_t *buf, size_t len, size_t pos) = 0; 
    virtual int remove(const char *path) { return 0; };
    virtual int truncate(const char *path, size_t len) { return 0; };
    virtual void close() {};
};

class ProcFsConnection : public StorageInterface {
public:
    struct ProcFsFile { 
        string name;
        std::function<void(string &s)> read;
        std::function<void(const string &s)> write;
    };
    vector<ProcFsFile> files;
private:
    int curFile = -1;
    string curIo;
    int curMode;
    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>func) {
        for (auto f : files) { 
            struct dirent de = {0};
            strncpy(de.d_name, f.name.c_str(), sizeof(de.d_name));
            de.d_type = DT_REG;
            func(0, &de, 100);
        }
        return files.size();
    };
    int open(const char *path, int mode, int ac = 0) {
        for (int n = 0; n < files.size(); n++) {  
            if (path == files[n].name) { 
                curFile = n;
                curIo = "";
                curMode = mode;
                if (mode == O_RDONLY)   
                    files[curFile].read(curIo);
                return 0;
            }
        }
        return -ENOENT;
    };
    // todo add "bool eof" argument to function 
    int pwrite(const uint8_t *buf, size_t len, size_t pos, bool eof) {
        curIo += string(buf, buf + len);
        if (eof) 
            files[curFile].write(curIo);
        return len;
    }; 
    int pread(uint8_t *buf, size_t len, size_t pos) {
        if (pos >= curIo.length()) {
            len = 0;
        } else { 
            len = std::min(curIo.length() - pos, len);
            memcpy(buf, curIo.c_str() + pos, len);
        }
        return len;
    }; 
public:
    ProcFsConnection(const char *url) {}
    void add(const ProcFsFile &f) { files.push_back(f); }
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
        wifiRun();
        smb2 = smb2_init_context();
        if (password != "")
            smb2_set_password(smb2, password.c_str());
        url = smb2_parse_url(smb2, urlString.c_str());
        smb2_set_timeout(smb2, 3/*seconds*/);
        if (smb2_connect_share(smb2, url->server, url->share, url->user) < 0) {
            printf("smb2_connect_share failed. %s\n", smb2_get_error(smb2));
        }
    }
    int remove(const char *path) { 
        cacheCloseAll();
        int r = smb2_unlink(smb2, path);
        if (r < 0) 
            printf("smb2_unlink failed. %s\n", smb2_get_error(smb2));
        return r;
    }
    int truncate(const char *path, size_t len) { 
        int r = smb2_truncate(smb2, path, len);
        if (r < 0) 
            printf("smb2_unlink failed. %s\n", smb2_get_error(smb2));
        return r;
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

    static const int cacheSize = 4;
    struct CacheEntry {
        struct smb2fh *fh = NULL;
        string name;
        int mode = 0;
        void clear() { name = ""; mode = 0; fh = NULL; }    
    } fhCache[cacheSize];

    int cacheMisses = 0;
    struct smb2fh *cacheOpen(const char *path, int mode) {
        if (fhCache[0].name == path && fhCache[0].mode == mode)
            return fhCache[0].fh;
        for(int i = 1; i < cacheSize; i++) {
            if(fhCache[i].name == path && fhCache[i].mode == mode) {
                CacheEntry ce = fhCache[i];
                for(int j = i; j > 0; j--) 
                    fhCache[j] = fhCache[j - 1]; 
                fhCache[0] = ce;
                return fhCache[0].fh;
            }
        }
        struct smb2fh *f = smb2_open(smb2, path, mode);
        if (f == NULL) {
            printf("smb2_open failed. %s\n", smb2_get_error(smb2));
            return NULL;
        }
        cacheMisses++;
        //printf("cacheOpen misses %d\n", cacheMisses);
        if(fhCache[cacheSize - 1].fh != NULL)
            smb2_close(smb2, fhCache[cacheSize - 1].fh);
        for(int j = cacheSize - 1; j > 0; j--) 
            fhCache[j] = fhCache[j - 1]; 
        fhCache[0].fh = f;
        fhCache[0].name = path;
        fhCache[0].mode = mode;
        return f;
    }

    void cacheCloseAll() { 
        for(int i = 0; i < cacheSize; i++) {
            if(fhCache[i].fh != NULL)
                smb2_close(smb2, fhCache[i].fh);
            fhCache[i].clear();
        }
    }

    int open(const char *path, int mode, int perm = 0) { 
        LOG("open('%s')\n", path);
        fh = cacheOpen(path, mode);
        return fh == NULL ? -1 : 0;
    }
    int pwrite(const uint8_t *buf, size_t len, size_t pos, bool eof) { 
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

    // convert filename into 8+3 uppercase, padded with ' ' 
    // TODO: handle name conflicts, map illegal characters  
    void setFilename(const char *fn) { 
        const char *dot = strrchr(fn, '.');
        if (dot == NULL) 
            dot = fn + std::min(strlen(fn), sizeof(filename));
        for(int n = 0; n < dot - fn; n++) filename[n] = toupper(fn[n]);
        for(int n = dot - fn; n < sizeof(filename); n++) filename[n] = ' ';        
        for(int n = 0; n < sizeof(extension); n++) 
            extension[n] = (dot + 1 + n) < fn + strlen(fn) ? toupper(*(dot + 1 + n)) : ' ';
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
    StorageInterface *io;

    bool stitchDir(const char *path) {
        rootPath = path;
        int filesStitched = 0;
        LOG("stitchDir('%s')\n", path);
        io->readdir(path, [this,path,&filesStitched](int n, dirent *entry, size_t len){
            //LOG("readdir %s\n", entry->d_name);
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
        if (de < 0) { 
            LOG("stitchDir(): no free dirent for file '%s'\n", fn);
            return false;
        }
        if (sectorCount > vtoc.freeSectors) {
            LOG("stitchDir(): insuf free sect for file '%s' (%d>%d)\n", fn, sectorCount, vtoc.freeSectors);
            return false;
        }
        Dos2Dirent *dirent = &dirSectors[de];
        newFile.fileNo = de;
        newFile.len = fileLen;
        newFile.flags = Dos2Dirent::dos2 | Dos2Dirent::inUse;
        LOG("allocating for file '%s', fileLen %d sectorCount %d\n", fn, (int)fileLen, sectorCount);
        for(int i = 0; i < sectorCount; i++) { 
        //for(int i = 0; i < 1; i++) { // TEST: lazily allocate sectors in this->read().  Doesn't work XDOS caches the vtoc
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
    DiskStitchImage(StorageInterface *p) : io(p) {
        for(int i = 0; i < sizeof(vtoc.bitmap)/sizeof(vtoc.bitmap[0]); i++) vtoc.bitmap[i] = -1;
        for(int i = 0; i <= 3; i++) vtoc.setBitmap(i, false);
        for(int i = 360; i <= 368; i++) vtoc.setBitmap(i, false);
        sector1.sectorSize = sectorSize();
        vtoc.freeSectors = vtoc.usableSectors = sectorCount() - 13;
    }

    bool started = false;
    void start() { 
        io->connect();
        stitchDir("");
        started = true;
    }

    int sectorDataSize() { return sectorSize() - 3; }
    int specificSectorSize(int s) { return (s <= 3) ? 128 : sectorSize(); }
    int offsetToSector(int off) { 
        return (off < 128 * 3) ? (off / 128) + 1 : (off - 3 * 128) / sectorSize() + 4;  
    } 
    int sectorToOffset(int s) { return s <= 3 ? (s - 1) * 128 : (s - 4) * sectorSize() + 3 * 128; }

    size_t read(uint8_t *buf, size_t sector) {
        if (!started) start();
        LOG("DiskStitchImage::read() sector %d\n", (int)sector);
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
                
                int r = io->open(fn.c_str(), O_RDONLY);
                if (r < 0) {
                    printf("error opening file '%s', errno=%d\n", fn.c_str(), errno);
                    return r;
                }
                int len = io->pread(buf, sectorDataSize(), fsec * sectorDataSize());
                if (convertAtascii) 
                    charConvert(buf, '\n', '\233');
                io->close();
                if (len < 0) {
                    printf("error reading file '%s'\n", fn.c_str());
                    return len;
                }
                int nextSector = sf->sectors.size() > fsec + 1 ? (sf->sectors[fsec + 1]) : 0;
                // TODO: this doesn't work, XDOS caches the vtoc so we can't change it after first access
                if (0 && nextSector == 0 && sf->len > (fsec + 1) * sectorDataSize()) {
                    nextSector = vtoc.findFreeSector();
                    sf->sectors.push_back(nextSector);
                    vtoc.setBitmap(nextSector, false);
                    LOG("DiskStitchImage::read() extending stitched file '%s' from fsec %d with new sector %d\n", 
                        sf->hostFile.c_str(), fsec, nextSector);
                }
                //printf("reading sector %d for file '%s' fsec %d, len %d\n", (int)sector, fn.c_str(), fsec, len); 
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
        LOG("DiskStitchImage::write() sector %d\n", (int)sector);
        if(sector == 360) {
            //printf("Writing VTOC.  Before write:\n");
            //vtoc.printBitmap();
            memcpy(&vtoc, buf, sizeof(vtoc));
            //printf("Writing VTOC.  After write:\n");
            //vtoc.printBitmap();
        } else if(sector >= 361 && sector <= 368) {
            // first dirent in the sector we're reading
            int secStartDirent = (sector - 361) * direntSecSize / sizeof(dirSectors[0]); 
            memcpy(&dirSectors[secStartDirent], buf, direntSecSize); //sectorSize());
            for(int n = 0; n < numDirent; n++) {
                Dos2Dirent *d = &dirSectors[n];
                StitchedFile *sf = &stitchedFiles[n];
                if (0 && d->flags != 0) {
                    LOG("dirent%02d: ", n);
                    d->print();
                }
                if (d->flags == stitchedFiles[n].flags) 
                    continue;

                string filename = d->getFilename();
                LOG("dirent %02d '%s' flags changed %02x!=%02x\n", n, filename.c_str(), d->flags, stitchedFiles[n].flags);
                //d->print();
                // TODO: file truncated by writing flags to 0x43 and d->count to 1, we should trim
                //   sf->sectors[], look into removing the field sf->len, and call truncate(2)
                // TODO: file erased by writing flags to 0x80, we should call rm(2) 
                // TODO: here is where we could delete a file or notice a new file created
                // TODO: change stitchedFiles from a vector to an array sized to match the disk dir table
                if (d->flags == Dos2Dirent::deleted) {
                    LOG("deleting '%s'\n", sf->hostFile.c_str());
                    io->remove(sf->hostFile.c_str());
                    sf->flags = 0;
                    sf->sectors.clear();
                    sf->len = 0;
                    sf->hostFile = "";
                }
                
                if ((d->flags & Dos2Dirent::inUse) != 0 && (sf->flags & Dos2Dirent::inUse) == 0) { 
                    //printf("new dirent: ");
                    //d->print();
                    StitchedFile newFile;
                    newFile.fileNo = n;
                    newFile.flags = d->flags;
                    newFile.sectors.push_back(d->startSector);
                    string filename;
                    for(int n = 0; n < sizeof(d->filename); n++) {
                        if (d->filename[n] != ' ') filename += d->filename[n];
                    }
                    newFile.hostFile = filename;
                    LOG("creating '%s'\n", newFile.hostFile.c_str());
                    int fd = io->open(newFile.hostFile.c_str(), O_CREAT | O_RDWR, 0644);
                    if (fd < 0) {
                        printf("error creating '%s'\n", newFile.hostFile.c_str());
                    } else { 
                        io->close();
                    }
                    *sf = newFile;
                }
                sf->flags = d->flags;
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
                string &fn = sf->hostFile;
                
                int r = io->open(fn.c_str(), O_RDWR);
                if (r < 0) {
                    printf("error opening file '%s', errno=%d\n", fn.c_str(), errno);
                    return r;
                }
                bool eof = (nextSector == 0);
                if (convertAtascii) { 
                    static uint8_t buf2[512]; // TODO
                    memcpy(buf2, buf, sectorSize());
                    charConvert(buf2, '\233', '\n');
                    r = io->pwrite(buf2, len, fsec * sectorDataSize(), eof);
                } else { 
                    r = io->pwrite(buf, len, fsec * sectorDataSize(), eof);
                }
                io->close();
                if (r < 0) {
                    printf("error writing file '%s'\n", fn.c_str());
                    return r;
                }
                // writing past end of file, extend sf->sectors
                if (nextSector != 0 && sf->sectors.size() == fsec + 1) 
                    sf->sectors.push_back(nextSector);
                // if nextSector == 0 we trim sf->sectors[] and call truncate(2)
                if (nextSector == 0 && sf->sectors.size() > fsec + 1) {
                    io->truncate(sf->hostFile.c_str(), len);
                    sf->sectors.resize(fsec + 1);
                }
            }
        }
        return sectorSize();
    };
    int sectorSize() { return SECTOR_SIZE; } //XXSECSIZE  
    int sectorCount() { return 720; }
    bool valid() { return true; }
};

template<class IO> 
class DiskStitchGeneric : public DiskStitchImage {
public:
    IO storage;
    DiskStitchGeneric(const char *url = NULL) : DiskStitchImage(&storage), storage(url) {}
};

class DiskProcFs : public DiskStitchGeneric<ProcFsConnection> { 
    public:
    DiskProcFs(const vector<ProcFsConnection::ProcFsFile> &a) { 
        storage.files = a; 
        start(); 
    };
};
