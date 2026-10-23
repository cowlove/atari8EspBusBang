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
#include <algorithm>
#include <map>
#include <set>

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 256
#endif

#include "diskImage.h"
using std::string;
using std::vector;

struct AtariFilename {
    char name[8];
    char extension[3];
    string key;
    bool visible;
    bool aliased;
    AtariFilename() : visible(false), aliased(false) {
        memset(name, ' ', sizeof(name));
        memset(extension, ' ', sizeof(extension));
    }
};

inline bool atariFilenameChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '@';
}

inline bool atariAliasChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

inline string atariFilenameKey(const char name[8], const char extension[3]) {
    string key(name, name + 8);
    key.append(extension, extension + 3);
    return key;
}

inline bool encodeAtariFilename(const string &hostName, AtariFilename &result) {
    const size_t dot = hostName.find_last_of('.');
    const size_t baseLength = dot == string::npos ? hostName.size() : dot;
    const size_t extensionLength = dot == string::npos ? 0 : hostName.size() - dot - 1;
    if (!baseLength || baseLength > 8 || extensionLength > 3) return false;
    for (size_t i = 0; i < hostName.size(); ++i) {
        if (i == dot) continue;
        unsigned char c = static_cast<unsigned char>(hostName[i]);
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (!atariFilenameChar(c)) return false;
        if (i < baseLength) result.name[i] = c;
        else result.extension[i - dot - 1] = c;
    }
    result.key = atariFilenameKey(result.name, result.extension);
    return true;
}

inline bool atariFilenameLess(const string &left, const string &right) {
    const size_t length = std::min(left.size(), right.size());
    for (size_t i = 0; i < length; ++i) {
        unsigned char a = static_cast<unsigned char>(left[i]);
        unsigned char b = static_cast<unsigned char>(right[i]);
        unsigned char upperA = a >= 'a' && a <= 'z' ? a - ('a' - 'A') : a;
        unsigned char upperB = b >= 'a' && b <= 'z' ? b - ('a' - 'A') : b;
        if (upperA != upperB) return upperA < upperB;
    }
    if (left.size() != right.size()) return left.size() < right.size();
    return left < right;
}

inline vector<AtariFilename> assignAtariFilenames(const vector<string> &hostNames) {
    vector<AtariFilename> result(hostNames.size());
    vector<size_t> order(hostNames.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&hostNames](size_t left, size_t right) {
        return atariFilenameLess(hostNames[left], hostNames[right]);
    });

    std::set<string> reserved;
    vector<bool> direct(hostNames.size(), false);
    for (size_t i = 0; i < order.size(); ++i) {
        size_t index = order[i];
        AtariFilename candidate;
        if (encodeAtariFilename(hostNames[index], candidate) &&
            reserved.insert(candidate.key).second) {
            candidate.visible = true;
            result[index] = candidate;
            direct[index] = true;
        }
    }

    std::map<string, vector<size_t> > aliasGroups;
    for (size_t i = 0; i < order.size(); ++i) {
        size_t index = order[i];
        if (direct[index]) continue;
        const string &host = hostNames[index];
        size_t dot = host.find_last_of('.');
        size_t baseLength = dot == string::npos ? host.size() : dot;
        string base;
        for (size_t j = 0; j < baseLength && base.size() < 7; ++j) {
            unsigned char c = static_cast<unsigned char>(host[j]);
            if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
            if (atariAliasChar(c)) base += static_cast<char>(c);
        }
        if (base.empty()) base = "FILE";
        string extension;
        if (dot != string::npos) {
            for (size_t j = dot + 1; j < host.size() && extension.size() < 3; ++j) {
                unsigned char c = static_cast<unsigned char>(host[j]);
                if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
                if (atariAliasChar(c)) extension += static_cast<char>(c);
            }
        }
        string group = base + "." + extension;
        aliasGroups[group].push_back(index);
    }

    for (std::map<string, vector<size_t> >::iterator group = aliasGroups.begin();
         group != aliasGroups.end(); ++group) {
        size_t nextNumber = 1;
        for (size_t i = 0; i < group->second.size() && nextNumber <= 9; ++i) {
            size_t index = group->second[i];
            bool assigned = false;
            while (nextNumber <= 9 && !assigned) {
                AtariFilename candidate;
                memcpy(candidate.name, group->first.data(),
                       std::min<size_t>(7, group->first.find('.')));
                candidate.name[std::min<size_t>(7, group->first.find('.'))] = '0' + nextNumber++;
                size_t separator = group->first.find('.');
                string extension = group->first.substr(separator + 1);
                memcpy(candidate.extension, extension.data(), extension.size());
                candidate.key = atariFilenameKey(candidate.name, candidate.extension);
                if (!reserved.count(candidate.key)) {
                    candidate.visible = true;
                    candidate.aliased = true;
                    reserved.insert(candidate.key);
                    result[index] = candidate;
                    assigned = true;
                }
            }
        }
    }
    return result;
}

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

    void setFilename(const AtariFilename &mappedName) {
        memcpy(filename, mappedName.name, sizeof(filename));
        memcpy(extension, mappedName.extension, sizeof(extension));
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
        struct HostEntry {
            string name;
            size_t length;
        };
        vector<HostEntry> entries;
        LOG("stitchDir('%s')\n", path);
        int result = io->readdir(path, [&entries](int, dirent *entry, size_t len){
            if (!entry) return;
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                return;
            HostEntry host = { entry->d_name, len };
            entries.push_back(host);
        }); 
        if (result < 0) return false;
        std::sort(entries.begin(), entries.end(), [](const HostEntry &left, const HostEntry &right) {
            return atariFilenameLess(left.name, right.name);
        });
        vector<string> hostNames;
        for (size_t i = 0; i < entries.size(); ++i) hostNames.push_back(entries[i].name);
        vector<AtariFilename> mappedNames = assignAtariFilenames(hostNames);
        int filesStitched = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!mappedNames[i].visible) {
                LOG("DOS 2.0: omitting '%s' (no free one-digit 8.3 alias)\n", entries[i].name.c_str());
                continue;
            }
            if (entries[i].length > vtoc.freeSectors * sectorDataSize()) continue;
            if (mappedNames[i].aliased)
                LOG("DOS 2.0: alias '%s' as '%.8s.%.3s'\n", entries[i].name.c_str(),
                    mappedNames[i].name, mappedNames[i].extension);
            if (stitchFile(entries[i].name.c_str(), entries[i].length, mappedNames[i]))
                ++filesStitched;
        }
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

    bool stitchFile(const char *fn, size_t fileLen, const AtariFilename &mappedName) {
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
        dirent->setFilename(mappedName);
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
    int specificSectorSize(int s) override { return (s <= 3) ? 128 : sectorSize(); }
    int offsetToSector(int off) override { 
        return (off < 128 * 3) ? (off / 128) + 1 : (off - 3 * 128) / sectorSize() + 4;  
    } 
    int sectorToOffset(int s) override { return s <= 3 ? (s - 1) * 128 : (s - 4) * sectorSize() + 3 * 128; }

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
    int write(const uint8_t *buf, size_t sector) {
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
            const Dos2Dirent *incoming = (const Dos2Dirent *)buf;
            int entriesInSector = direntSecSize / sizeof(dirSectors[0]);
            for (int i = 0; i < entriesInSector; ++i) {
                bool active = (incoming[i].flags & Dos2Dirent::inUse) != 0;
                bool wasActive = (dirSectors[secStartDirent + i].flags & Dos2Dirent::inUse) != 0;
                if (active && !wasActive) {
                    string filename;
                    for (int j = 0; j < sizeof(incoming[i].filename); ++j) {
                        if (incoming[i].filename[j] != ' ')
                            filename += incoming[i].filename[j];
                    }
                    int fd = io->open(filename.c_str(), O_CREAT | O_RDWR, 0644);
                    if (fd < 0) {
                        LOG("creation of '%s' rejected as write-protected\n", filename.c_str());
                        return -EROFS;
                    }
                    io->close();
                }
            }
            memcpy(&dirSectors[secStartDirent], buf, direntSecSize); //sectorSize());
            for(int n = 0; n < numDirent; n++) {
                Dos2Dirent *d = &dirSectors[n];
                StitchedFile *sf = &stitchedFiles[n];
                if (0 && d->flags != 0) {
                    LOG("dirent%02d: ", n);
                    d->print();
                }
                bool active = (d->flags & Dos2Dirent::inUse) != 0;
                bool wasActive = (sf->flags & Dos2Dirent::inUse) != 0;
                bool startChanged = active && wasActive &&
                    (sf->sectors.empty() || d->startSector != sf->sectors.front());
                bool sectorCountChanged = active && wasActive &&
                    d->sectorCount != sf->sectors.size();
                if (d->flags == sf->flags && !startChanged && !sectorCountChanged)
                    continue;

                string filename = d->getFilename();
                LOG("dirent %02d '%s' changed flags %02x->%02x start %d count %d\n",
                    n, filename.c_str(), sf->flags, d->flags,
                    d->startSector, d->sectorCount);
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
                        return -EROFS;
                    } else { 
                        io->close();
                    }
                    *sf = newFile;
                } else if (active && wasActive) {
                    if (startChanged) {
                        sf->sectors.clear();
                        sf->sectors.push_back(d->startSector);
                        sf->len = 0;
                    } else if (d->sectorCount < sf->sectors.size()) {
                        sf->sectors.resize(std::max<size_t>(d->sectorCount, 1));
                    }
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
                size_t writeLen = len == 0 ? sectorDataSize() : len;
                if (convertAtascii) { 
                    static uint8_t buf2[512]; // TODO
                    memcpy(buf2, buf, sectorSize());
                    charConvert(buf2, '\233', '\n');
                    r = io->pwrite(buf2, writeLen, fsec * sectorDataSize(), eof);
                } else { 
                    r = io->pwrite(buf, writeLen, fsec * sectorDataSize(), eof);
                }
                io->close();
                if (r < 0) {
                    printf("error writing file '%s'\n", fn.c_str());
                    return r;
                }
                // writing past end of file, extend sf->sectors
                if (nextSector != 0) {
                    if (sf->sectors.size() == fsec + 1) {
                        sf->sectors.push_back(nextSector);
                    } else if (sf->sectors.size() > fsec + 1 &&
                               sf->sectors[fsec + 1] != nextSector) {
                        sf->sectors.resize(fsec + 1);
                        sf->sectors.push_back(nextSector);
                    }
                }
                if (eof) {
                    size_t finalSize = fsec * sectorDataSize() + writeLen;
                    int truncateResult = io->truncate(sf->hostFile.c_str(), finalSize);
                    if (truncateResult < 0) {
                        printf("error truncating file '%s'\n", fn.c_str());
                        return truncateResult;
                    }
                    if (sf->sectors.size() > fsec + 1)
                        sf->sectors.resize(fsec + 1);
                    sf->len = finalSize;
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
