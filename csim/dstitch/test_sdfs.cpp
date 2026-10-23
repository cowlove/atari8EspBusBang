#define DRAM_ATTR
#include "../../main/diskStitchImageSDFS.h"
#include <assert.h>
#include <map>

class TestStorage : public StorageInterface {
    std::map<string, vector<uint8_t> > contents;
    string current;
public:
    explicit TestStorage(bool empty = false, size_t bigFileSize = 127 * 256) {
        if (empty) return;
        contents["ALPHA.TXT"] = vector<uint8_t>(260);
        for (size_t i = 0; i < contents["ALPHA.TXT"].size(); ++i)
            contents["ALPHA.TXT"][i] = static_cast<uint8_t>(i);
        contents["BIGFILE.BIN"] = vector<uint8_t>(bigFileSize);
        for (size_t i = 0; i < contents["BIGFILE.BIN"].size(); ++i)
            contents["BIGFILE.BIN"][i] = static_cast<uint8_t>(i * 7);
        contents["ZERO.DAT"] = vector<uint8_t>();
    }
    int readdir(const char *, std::function<void(int, dirent *, size_t)> callback) override {
        int index = 0;
        for (std::map<string, vector<uint8_t> >::iterator i = contents.begin(); i != contents.end(); ++i) {
            dirent entry = {};
            strncpy(entry.d_name, i->first.c_str(), sizeof(entry.d_name) - 1);
            callback(index++, &entry, i->second.size());
        }
        return index;
    }
    int open(const char *path, int mode, int = 0) override {
        current = path;
        if (contents.count(current)) return (mode & O_EXCL) ? -EEXIST : 1;
        if (mode & O_CREAT) {
            contents[current] = vector<uint8_t>();
            return 1;
        }
        return -ENOENT;
    }
    int pwrite(const uint8_t *buf, size_t length, size_t offset, bool) override {
        if (current.empty()) return -EBADF;
        vector<uint8_t> &data = contents[current];
        if (offset + length > data.size()) data.resize(offset + length);
        memcpy(data.data() + offset, buf, length);
        return length;
    }
    int pread(uint8_t *buf, size_t length, size_t offset) override {
        vector<uint8_t> &data = contents[current];
        if (offset >= data.size()) return 0;
        length = std::min(length, data.size() - offset);
        memcpy(buf, data.data() + offset, length);
        return length;
    }
    size_t fileSize(const char *name) const { return contents.at(name).size(); }
    uint8_t fileByte(const char *name, size_t offset) const { return contents.at(name).at(offset); }
    bool hasFile(const char *name) const { return contents.count(name) != 0; }
    void close() override { current.clear(); }
};

class AliasStorage : public StorageInterface {
    std::map<string, vector<uint8_t> > contents;
    string current;
public:
    AliasStorage() {
        contents["LONGFILENAME.TXT"] = vector<uint8_t>(1, 'a');
        contents["LONGFILENAMEAGAIN.TXT"] = vector<uint8_t>(1, 'b');
        contents["LONGFIL1.TXT"] = vector<uint8_t>(1, 'c');
    }
    int readdir(const char *, std::function<void(int, dirent *, size_t)> callback) override {
        int index = 0;
        for (std::map<string, vector<uint8_t> >::iterator i = contents.begin(); i != contents.end(); ++i) {
            dirent entry = {};
            strncpy(entry.d_name, i->first.c_str(), sizeof(entry.d_name) - 1);
            callback(index++, &entry, i->second.size());
        }
        return index;
    }
    int open(const char *path, int, int = 0) override {
        current = path;
        return contents.count(current) ? 1 : -ENOENT;
    }
    int pwrite(const uint8_t *, size_t, size_t, bool) override { return -EROFS; }
    int pread(uint8_t *buf, size_t length, size_t offset) override {
        vector<uint8_t> &data = contents[current];
        if (offset >= data.size()) return 0;
        length = std::min(length, data.size() - offset);
        memcpy(buf, data.data() + offset, length);
        return length;
    }
    void close() override { current.clear(); }
};

static uint16_t get16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t get24(const uint8_t *p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
}

static void put16(uint8_t *p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

static void put24(uint8_t *p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
}

static void setCreateEntry(uint8_t *directory, size_t offset, const char *name, uint16_t mapSector) {
    uint8_t *entry = directory + offset;
    memset(entry, 0, 23);
    entry[0] = 0x88;
    put16(entry + 1, mapSector);
    memset(entry + 6, ' ', 8);
    memcpy(entry + 6, name, std::min<size_t>(8, strlen(name)));
    memset(entry + 14, ' ', 3);
}

int main() {
    vector<string> aliasInputs;
    aliasInputs.push_back("LONGFILENAME.TXT");
    aliasInputs.push_back("LONGFILENAMEAGAIN.TXT");
    aliasInputs.push_back("LONGFIL1.TXT");
    aliasInputs.push_back("LONGFIL2.TXT");
    vector<AtariFilename> aliases = assignAtariFilenames(aliasInputs);
    assert(aliases[0].visible && aliases[0].aliased);
    assert(aliases[1].visible && aliases[1].aliased);
    assert(aliases[2].visible && !aliases[2].aliased);
    assert(aliases[3].visible && !aliases[3].aliased);
    assert(memcmp(aliases[0].name, "LONGFIL3", 8) == 0);
    assert(memcmp(aliases[1].name, "LONGFIL4", 8) == 0);
    assert(memcmp(aliases[0].extension, "TXT", 3) == 0);

    vector<string> crowdedInputs;
    for (int i = 0; i < 10; ++i) {
        char filename[32];
        snprintf(filename, sizeof(filename), "LONGFILENAME%02d.TXT", i);
        crowdedInputs.push_back(filename);
    }
    vector<AtariFilename> crowdedAliases = assignAtariFilenames(crowdedInputs);
    int visibleAliases = 0;
    for (size_t i = 0; i < crowdedAliases.size(); ++i)
        if (crowdedAliases[i].visible) ++visibleAliases;
    assert(visibleAliases == 9);

    AliasStorage aliasStorage;
    DiskStitchImageSDFS aliasSdfs(&aliasStorage, 2048);
    assert(aliasSdfs.valid());
    uint8_t aliasSector[256];
    assert(aliasSdfs.read(aliasSector, 1) == 128);
    uint16_t aliasRootMap = get16(&aliasSector[9]);
    assert(aliasSdfs.read(aliasSector, aliasRootMap) == 256);
    uint16_t aliasRootData = get16(&aliasSector[4]);
    assert(aliasSdfs.read(aliasSector, aliasRootData) == 256);
    uint8_t aliasRootDirectory[256];
    memcpy(aliasRootDirectory, aliasSector, sizeof(aliasRootDirectory));
    assert(memcmp(&aliasSector[29], "LONGFIL2", 8) == 0 ||
           memcmp(&aliasSector[52], "LONGFIL2", 8) == 0 ||
           memcmp(&aliasSector[75], "LONGFIL2", 8) == 0);
    assert(memcmp(&aliasSector[29], "LONGFIL3", 8) == 0 ||
           memcmp(&aliasSector[52], "LONGFIL3", 8) == 0 ||
           memcmp(&aliasSector[75], "LONGFIL3", 8) == 0);
    assert(memcmp(&aliasSector[29], "LONGFIL1", 8) == 0);
    bool sdfsAliasDataA = false;
    bool sdfsAliasDataB = false;
    bool sdfsTrueData = false;
    for (size_t offset = 23; offset < 23 * 4; offset += 23) {
        uint16_t mapSector = get16(&aliasRootDirectory[offset + 1]);
        assert(mapSector != 0);
        assert(aliasSdfs.read(aliasSector, mapSector) == 256);
        uint16_t dataSector = get16(&aliasSector[4]);
        assert(dataSector != 0);
        assert(aliasSdfs.read(aliasSector, dataSector) == 256);
        if (memcmp(&aliasRootDirectory[offset + 6], "LONGFIL2", 8) == 0 && aliasSector[0] == 'a')
            sdfsAliasDataA = true;
        if (memcmp(&aliasRootDirectory[offset + 6], "LONGFIL3", 8) == 0 && aliasSector[0] == 'b')
            sdfsAliasDataB = true;
        if (memcmp(&aliasRootDirectory[offset + 6], "LONGFIL1", 8) == 0 && aliasSector[0] == 'c')
            sdfsTrueData = true;
    }
    assert(sdfsAliasDataA && sdfsAliasDataB && sdfsTrueData);

    DiskStitchImage aliasDos2(&aliasStorage);
    uint8_t dosDirectorySector[256];
    assert(aliasDos2.read(dosDirectorySector, 361) == 256);
    bool dosHasAlias2 = false;
    bool dosHasAlias3 = false;
    bool dosHasTrueName = false;
    int alias2Start = 0;
    int alias3Start = 0;
    int trueStart = 0;
    for (int i = 0; i < 8; ++i) {
        Dos2Dirent *entry = reinterpret_cast<Dos2Dirent *>(dosDirectorySector) + i;
        if (memcmp(entry->filename, "LONGFIL2", 8) == 0) {
            dosHasAlias2 = true;
            alias2Start = entry->startSector;
        }
        if (memcmp(entry->filename, "LONGFIL3", 8) == 0) {
            dosHasAlias3 = true;
            alias3Start = entry->startSector;
        }
        if (memcmp(entry->filename, "LONGFIL1", 8) == 0) {
            dosHasTrueName = true;
            trueStart = entry->startSector;
        }
    }
    assert(dosHasAlias2 && dosHasAlias3 && dosHasTrueName);
    assert(aliasDos2.read(dosDirectorySector, alias2Start) == 256 && dosDirectorySector[0] == 'a');
    assert(aliasDos2.read(dosDirectorySector, alias3Start) == 256 && dosDirectorySector[0] == 'b');
    assert(aliasDos2.read(dosDirectorySector, trueStart) == 256 && dosDirectorySector[0] == 'c');

    TestStorage storage;
    DiskStitchImageSDFS disk(&storage);
    assert(disk.valid());
    assert(disk.sectorSize() == 256);
    assert(disk.sectorCount() == 65535);
    assert(disk.imageSize() == 3 * 128 + (65535 - 3) * 256);
    assert(disk.write(NULL, 1) == -EIO);

    uint8_t sector[256];
    assert(disk.read(sector, 1) == 128);
    uint16_t rootMap = get16(&sector[9]);
    assert(rootMap != 0);
    assert(get16(&sector[11]) == 65535);
    uint16_t freeSectors = get16(&sector[13]);
    assert(freeSectors == 65535 - 169);
    assert(disk.read(sector, 4) == 256);
    for (int i = 0; i <= 20; ++i) assert(sector[i] == 0x00);
    assert(sector[21] == 0x3f);

    assert(disk.read(sector, rootMap) == 256);
    uint16_t rootDirectorySector = get16(&sector[4]);
    assert(rootDirectorySector != 0);
    assert(disk.read(sector, rootDirectorySector) == 256);
    assert(get24(&sector[3]) == 92);
    assert(sector[23] == 0x08);
    uint16_t fileMap = get16(&sector[24]);
    uint16_t largeFileMap = get16(&sector[47]);
    uint8_t zeroEntry[23];
    memcpy(zeroEntry, &sector[69], sizeof(zeroEntry));
    assert(get24(&sector[26]) == 260);
    assert(memcmp(&sector[29], "ALPHA   ", 8) == 0);
    assert(memcmp(&sector[37], "TXT", 3) == 0);

    assert(disk.read(sector, fileMap) == 256);
    uint16_t firstDataSector = get16(&sector[4]);
    uint16_t secondDataSector = get16(&sector[6]);
    assert(firstDataSector != 0 && secondDataSector != 0);
    assert(disk.read(sector, firstDataSector) == 256);
    for (int i = 0; i < 256; ++i) assert(sector[i] == static_cast<uint8_t>(i));
    assert(disk.read(sector, secondDataSector) == 256);
    for (int i = 0; i < 4; ++i) assert(sector[i] == static_cast<uint8_t>(i));
    for (int i = 4; i < 256; ++i) assert(sector[i] == 0);

    assert(largeFileMap != 0);
    assert(disk.read(sector, largeFileMap) == 256);
    uint16_t nextMap = get16(&sector[0]);
    assert(nextMap != 0 && get16(&sector[2]) == 0);
    assert(disk.read(sector, nextMap) == 256);
    assert(get16(&sector[0]) == 0 && get16(&sector[2]) == largeFileMap);
    uint16_t lastDataSector = get16(&sector[4]);
    assert(lastDataSector != 0);
    assert(disk.read(sector, lastDataSector) == 256);
    for (int i = 0; i < 256; ++i)
        assert(sector[i] == static_cast<uint8_t>((126 * 256 + i) * 7));

    assert(get16(&zeroEntry[1]) == 0 && get24(&zeroEntry[3]) == 0);

    uint8_t replacement[256];
    memset(replacement, 0xa5, sizeof(replacement));
    assert(disk.write(replacement, firstDataSector) == 0);
    assert(disk.read(sector, firstDataSector) == 256);
    for (int i = 0; i < 256; ++i) assert(sector[i] == 0xa5);
    uint8_t changedMetadata[256];
    memset(changedMetadata, 0, sizeof(changedMetadata));
    assert(disk.write(changedMetadata, rootDirectorySector) == -EROFS);
    memset(replacement, 0x5a, sizeof(replacement));
    assert(disk.write(replacement, secondDataSector) == 0);
    assert(storage.fileSize("ALPHA.TXT") == 260);
    assert(disk.read(sector, secondDataSector) == 256);
    for (int i = 0; i < 4; ++i) assert(sector[i] == 0x5a);
    for (int i = 4; i < 256; ++i) assert(sector[i] == 0);

    DiskStitchImageSDFS smallerDisk(&storage, 32768);
    assert(smallerDisk.valid());
    assert(smallerDisk.sectorCount() == 32768);
    assert(smallerDisk.imageSize() == 3 * 128 + (32768 - 3) * 256);
    assert(smallerDisk.read(sector, 1) == 128);
    assert(get16(&sector[11]) == 32768);

    TestStorage overlayStorage;
    DiskStitchImageSDFS overlayDisk(&overlayStorage, 2048, "DSTITCH", true);
    assert(overlayDisk.valid());
    uint8_t overlayWrite[256];
    memset(overlayWrite, 0xc3, sizeof(overlayWrite));
    const uint16_t unallocatedSector = 2000;
    assert(overlayDisk.write(overlayWrite, unallocatedSector) == 0);
    memset(sector, 0, sizeof(sector));
    assert(overlayDisk.read(sector, unallocatedSector) == 256);
    for (int i = 0; i < 256; ++i) assert(sector[i] == 0xc3);
    memset(overlayWrite, 0x96, sizeof(overlayWrite));
    assert(overlayDisk.write(overlayWrite, 1) == 0);
    memset(sector, 0, sizeof(sector));
    assert(overlayDisk.read(sector, 1) == 128);
    for (int i = 0; i < 128; ++i) assert(sector[i] == 0x96);
    assert(overlayStorage.fileByte("ALPHA.TXT", 0) == 0);

    TestStorage traceStorage;
    DiskStitchImageSDFS traceDisk(&traceStorage, 2048, "DSTITCH", true);
    assert(traceDisk.valid());
    assert(traceDisk.read(sector, 1) == 128);
    uint16_t traceRootMap = get16(&sector[9]);
    assert(traceDisk.read(sector, traceRootMap) == 256);
    uint16_t traceDirectorySector = get16(&sector[4]);
    assert(traceDisk.read(sector, traceDirectorySector) == 256);
    uint32_t traceDirectoryLength = get24(&sector[3]);
    assert(traceDirectoryLength == 92);
    setCreateEntry(sector, traceDirectoryLength, "NEWFILE", 1900);
    put24(&sector[3], traceDirectoryLength + 23);
    assert(traceDisk.write(sector, traceDirectorySector) == 0);
    assert(!traceDisk.traceInterleaveDetectedForTest());

    uint8_t mapSectorData[256] = {};
    assert(traceDisk.write(mapSectorData, 1900) == 0);
    memset(overlayWrite, 0x31, sizeof(overlayWrite));
    assert(traceDisk.write(overlayWrite, 1901) == 0);
    memset(overlayWrite, 0x32, sizeof(overlayWrite));
    assert(traceDisk.write(overlayWrite, 1902) == 0);
    put16(&mapSectorData[4], 1901);
    put16(&mapSectorData[6], 1902);
    assert(traceDisk.write(mapSectorData, 1900) == 0);
    assert(!traceDisk.traceInterleaveDetectedForTest());
    assert(!traceDisk.traceHypothesisFailedForTest());

    TestStorage interleaveStorage;
    DiskStitchImageSDFS interleaveDisk(&interleaveStorage, 2048, "DSTITCH", true);
    assert(interleaveDisk.valid());
    assert(interleaveDisk.read(sector, 1) == 128);
    assert(interleaveDisk.read(sector, get16(&sector[9])) == 256);
    traceDirectorySector = get16(&sector[4]);
    assert(interleaveDisk.read(sector, traceDirectorySector) == 256);
    traceDirectoryLength = get24(&sector[3]);
    setCreateEntry(sector, traceDirectoryLength, "FIRST", 1900);
    setCreateEntry(sector, traceDirectoryLength + 23, "SECOND", 1901);
    put24(&sector[3], traceDirectoryLength + 46);
    assert(interleaveDisk.write(sector, traceDirectorySector) == 0);
    assert(interleaveDisk.traceInterleaveDetectedForTest());

    TestStorage writePocStorage(true);
    DiskStitchImageSDFS writePocDisk(&writePocStorage, 2048, "DSTITCH", false, true);
    assert(writePocDisk.valid());
    assert(writePocDisk.read(sector, 1) == 128);
    assert(writePocDisk.read(sector, get16(&sector[9])) == 256);
    traceDirectorySector = get16(&sector[4]);
    assert(writePocDisk.read(sector, traceDirectorySector) == 256);
    traceDirectoryLength = get24(&sector[3]);

    uint8_t changedBitmap[256];
    assert(writePocDisk.read(changedBitmap, 4) == 256);
    const uint16_t createSectors[] = { 1900, 1901, 1902 };
    for (size_t i = 0; i < sizeof(createSectors) / sizeof(createSectors[0]); ++i)
        changedBitmap[createSectors[i] / 8] &= static_cast<uint8_t>(~(0x80 >> (createSectors[i] & 7)));
    assert(writePocDisk.write(changedBitmap, 4) == 0);

    assert(writePocDisk.read(sector, traceDirectorySector) == 256);
    setCreateEntry(sector, traceDirectoryLength, "NEWFILE", 1900);
    put24(&sector[3], traceDirectoryLength + 23);
    assert(writePocDisk.write(sector, traceDirectorySector) == 0);
    memset(mapSectorData, 0, sizeof(mapSectorData));
    assert(writePocDisk.write(mapSectorData, 1900) == 0);
    memset(overlayWrite, 0x71, sizeof(overlayWrite));
    assert(writePocDisk.write(overlayWrite, 1901) == 0);
    assert(writePocDisk.read(sector, 1) == 128);
    put16(&sector[13], static_cast<uint16_t>(get16(&sector[13]) - 3));
    assert(writePocDisk.write(sector, 1) == 0);
    assert(writePocDisk.read(sector, traceDirectorySector) == 256);
    sector[traceDirectoryLength] = 0x08;
    put24(&sector[traceDirectoryLength + 3], 300);
    assert(writePocDisk.write(sector, traceDirectorySector) == 0);
    put16(&mapSectorData[4], 1901);
    put16(&mapSectorData[6], 1902);
    assert(writePocDisk.write(mapSectorData, 1900) == 0);
    assert(!writePocStorage.hasFile("NEWFILE"));
    memset(overlayWrite, 0x72, sizeof(overlayWrite));
    assert(writePocDisk.write(overlayWrite, 1902) == 0);
    assert(writePocStorage.hasFile("NEWFILE"));
    assert(writePocStorage.fileSize("NEWFILE") == 300);
    assert(writePocStorage.fileByte("NEWFILE", 0) == 0x71);
    assert(writePocStorage.fileByte("NEWFILE", 299) == 0x72);
    assert(writePocDisk.read(sector, traceDirectorySector) == 256);
    assert(sector[traceDirectoryLength] == 0x08);
    assert(writePocDisk.read(sector, 1900) == 256);
    assert(get16(&sector[4]) == 1901 && get16(&sector[6]) == 1902);
    assert(writePocDisk.read(sector, 1901) == 256 && sector[0] == 0x71);

    assert(writePocDisk.read(overlayWrite, 1902) == 256);
    memset(overlayWrite + 44, 0x73, 256 - 44);
    assert(writePocDisk.write(overlayWrite, 1902) == 0);
    assert(writePocDisk.read(sector, traceDirectorySector) == 256);
    put24(&sector[traceDirectoryLength + 3], 512);
    assert(writePocDisk.write(sector, traceDirectorySector) == 0);
    assert(writePocStorage.fileSize("NEWFILE") == 512);
    assert(writePocStorage.fileByte("NEWFILE", 299) == 0x72);
    assert(writePocStorage.fileByte("NEWFILE", 300) == 0x73);
    assert(writePocStorage.fileByte("NEWFILE", 511) == 0x73);

    assert(writePocDisk.read(sector, traceDirectorySector) == 256);
    assert(writePocDisk.write(sector, traceDirectorySector) == 0);
    memset(overlayWrite, 0x73, sizeof(overlayWrite));
    assert(writePocDisk.write(overlayWrite, 1903) == 0);
    assert(writePocDisk.read(changedBitmap, 4) == 256);
    for (uint16_t appendSector = 1903; appendSector <= 1904; ++appendSector)
        changedBitmap[appendSector / 8] &= static_cast<uint8_t>(~(0x80 >> (appendSector & 7)));
    assert(writePocDisk.write(changedBitmap, 4) == 0);
    assert(writePocDisk.read(sector, 1) == 128);
    put16(&sector[13], static_cast<uint16_t>(get16(&sector[13]) - 2));
    assert(writePocDisk.write(sector, 1) == 0);
    assert(writePocDisk.read(sector, traceDirectorySector) == 256);
    put24(&sector[traceDirectoryLength + 3], 1024);
    assert(writePocDisk.write(sector, traceDirectorySector) == 0);
    assert(writePocDisk.read(mapSectorData, 1900) == 256);
    put16(&mapSectorData[8], 1903);
    put16(&mapSectorData[10], 1904);
    assert(writePocDisk.write(mapSectorData, 1900) == 0);
    assert(writePocStorage.fileSize("NEWFILE") == 512);
    memset(overlayWrite, 0x74, sizeof(overlayWrite));
    assert(writePocDisk.write(overlayWrite, 1904) == 0);
    assert(writePocStorage.fileSize("NEWFILE") == 1024);
    assert(writePocStorage.fileByte("NEWFILE", 512) == 0x73);
    assert(writePocStorage.fileByte("NEWFILE", 768) == 0x74);
    assert(writePocDisk.read(sector, traceDirectorySector) == 256);
    assert(get24(&sector[traceDirectoryLength + 3]) == 1024);
    assert(writePocDisk.read(sector, 1900) == 256);
    assert(get16(&sector[8]) == 1903 && get16(&sector[10]) == 1904);
    assert(writePocDisk.read(sector, 1903) == 256 && sector[0] == 0x73);

    assert(writePocDisk.read(sector, traceDirectorySector) == 256);
    assert(writePocDisk.write(sector, traceDirectorySector) == 0);
    memset(overlayWrite, 0x75, sizeof(overlayWrite));
    assert(writePocDisk.write(overlayWrite, 1905) == 0);
    memset(overlayWrite, 0x76, sizeof(overlayWrite));
    assert(writePocDisk.write(overlayWrite, 1906) == 0);
    assert(writePocDisk.read(changedBitmap, 4) == 256);
    for (uint16_t appendSector = 1905; appendSector <= 1906; ++appendSector)
        changedBitmap[appendSector / 8] &= static_cast<uint8_t>(~(0x80 >> (appendSector & 7)));
    assert(writePocDisk.write(changedBitmap, 4) == 0);
    assert(writePocDisk.read(sector, 1) == 128);
    put16(&sector[13], static_cast<uint16_t>(get16(&sector[13]) - 2));
    assert(writePocDisk.write(sector, 1) == 0);
    assert(writePocDisk.read(sector, traceDirectorySector) == 256);
    put24(&sector[traceDirectoryLength + 3], 1536);
    assert(writePocDisk.write(sector, traceDirectorySector) == 0);
    assert(writePocDisk.read(mapSectorData, 1900) == 256);
    put16(&mapSectorData[12], 1906);
    put16(&mapSectorData[14], 1905);
    assert(writePocDisk.write(mapSectorData, 1900) == 0);
    assert(writePocDisk.traceHypothesisFailedForTest());
    assert(writePocStorage.fileSize("NEWFILE") == 1536);
    assert(writePocStorage.fileByte("NEWFILE", 1024) == 0x76);
    assert(writePocStorage.fileByte("NEWFILE", 1280) == 0x75);

    TestStorage multiMapStorage(false, 125 * 256);
    DiskStitchImageSDFS multiMapDisk(&multiMapStorage, 2048, "DSTITCH", false, true);
    assert(multiMapDisk.valid());
    assert(multiMapDisk.read(sector, 1) == 128);
    uint16_t multiMapRootMap = get16(&sector[9]);
    assert(multiMapDisk.read(sector, multiMapRootMap) == 256);
    uint16_t multiMapDirectorySector = get16(&sector[4]);
    assert(multiMapDisk.read(sector, multiMapDirectorySector) == 256);
    size_t bigFileEntry = 0;
    for (size_t offset = 23; offset + 23 <= get24(&sector[3]); offset += 23) {
        if (memcmp(&sector[offset + 6], "BIGFILE ", 8) == 0) {
            bigFileEntry = offset;
            break;
        }
    }
    assert(bigFileEntry != 0);
    uint16_t firstMapSector = get16(&sector[bigFileEntry + 1]);
    assert(firstMapSector != 0);
    uint8_t firstMap[256];
    assert(multiMapDisk.read(firstMap, firstMapSector) == 256);
    uint8_t freeVolume[128];
    assert(multiMapDisk.read(freeVolume, 1) == 128);
    uint16_t oldFreeSectors = get16(&freeVolume[13]);

    const uint16_t multiMapAllocated[] = {1900, 1901, 1902, 1903};
    assert(multiMapDisk.read(changedBitmap, 4) == 256);
    for (size_t i = 0; i < sizeof(multiMapAllocated) / sizeof(multiMapAllocated[0]); ++i)
        changedBitmap[multiMapAllocated[i] / 8] &=
            static_cast<uint8_t>(~(0x80 >> (multiMapAllocated[i] & 7)));
    assert(multiMapDisk.write(changedBitmap, 4) == 0);
    for (uint16_t dataSector = 1901; dataSector <= 1903; ++dataSector) {
        memset(overlayWrite, static_cast<int>(0xa0 + dataSector - 1900), sizeof(overlayWrite));
        assert(multiMapDisk.write(overlayWrite, dataSector) == 0);
    }
    uint8_t newMap[256] = {};
    put16(&newMap[2], firstMapSector);
    put16(&newMap[4], 1902);
    put16(&newMap[6], 1903);
    assert(multiMapDisk.write(newMap, 1900) == 0);
    assert(multiMapDisk.read(freeVolume, 1) == 128);
    put16(&freeVolume[13], static_cast<uint16_t>(oldFreeSectors - 4));
    assert(multiMapDisk.write(freeVolume, 1) == 0);
    assert(multiMapDisk.read(firstMap, firstMapSector) == 256);
    put16(&firstMap[0], 1900);
    put16(&firstMap[4 + 125 * 2], 1901);
    assert(multiMapDisk.write(firstMap, firstMapSector) == 0);
    assert(multiMapDisk.read(sector, multiMapDirectorySector) == 256);
    put24(&sector[bigFileEntry + 3], 128 * 256);
    assert(multiMapDisk.write(sector, multiMapDirectorySector) == 0);
    assert(multiMapStorage.fileSize("BIGFILE.BIN") == 128 * 256);
    assert(multiMapStorage.fileByte("BIGFILE.BIN", 125 * 256) == 0xa1);
    assert(multiMapStorage.fileByte("BIGFILE.BIN", 126 * 256) == 0xa2);
    assert(multiMapStorage.fileByte("BIGFILE.BIN", 127 * 256) == 0xa3);
    assert(multiMapDisk.read(sector, firstMapSector) == 256);
    assert(get16(&sector[0]) == 1900 && get16(&sector[4 + 125 * 2]) == 1901);
    assert(multiMapDisk.read(sector, 1900) == 256);
    assert(get16(&sector[2]) == firstMapSector && get16(&sector[4]) == 1902 &&
           get16(&sector[6]) == 1903);

    printf("SDFS sector-level smoke test passed\n");
    return 0;
}
