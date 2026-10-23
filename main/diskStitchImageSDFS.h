#pragma once

#include "diskStitch.h"
#include <map>
using std::map;

class DiskStitchImageSDFS : public DiskImage {
    struct File {
        string hostName;
        char name[8];
        char extension[3];
        uint32_t length;
        vector<uint16_t> dataSectors;
        vector<uint16_t> mapSectors;
    };

    struct DataRef {
        uint32_t fileIndex;
        uint32_t fileSector;
        bool mapped;
        DataRef() : fileIndex(0), fileSector(0), mapped(false) {}
    };

    static const int sdfsSectorSize = 256;
    static const int maxSectorCount = 65535;
    static const int directoryRecordSize = 23;
    StorageInterface *io;
    int sectors;
    int bitmapSectors;
    uint16_t bitmapStart;
    uint16_t rootMap;
    uint32_t allocationSearchStart;
    uint32_t allocatedCount;
    bool initialized;
    bool imageIsValid;
    string volumeName;
    vector<File> files;
    vector<uint8_t> allocated;
    vector<DataRef> dataRefs;
    vector<uint8_t> bitmap;
    map<uint16_t, vector<uint8_t> > metadata;

    static void put16(uint8_t *dst, uint16_t value) {
        dst[0] = value & 0xff;
        dst[1] = value >> 8;
    }

    static void put24(uint8_t *dst, uint32_t value) {
        dst[0] = value & 0xff;
        dst[1] = (value >> 8) & 0xff;
        dst[2] = (value >> 16) & 0xff;
    }

    uint16_t allocateSector() {
        while (allocationSearchStart <= sectors && allocated[allocationSearchStart])
            ++allocationSearchStart;
        if (allocationSearchStart > sectors) return 0;
        uint16_t result = allocationSearchStart++;
        allocated[result] = 1;
        ++allocatedCount;
        return result;
    }

    bool allocateSectors(size_t count, vector<uint16_t> &result) {
        result.clear();
        if (count > static_cast<size_t>(sectors)) return false;
        for (size_t i = 0; i < count; ++i) {
            uint16_t sector = allocateSector();
            if (!sector) return false;
            result.push_back(sector);
        }
        return true;
    }

    bool buildMap(const vector<uint16_t> &data, vector<uint16_t> &maps) {
        const size_t entriesPerMap = (sdfsSectorSize - 4) / 2;
        size_t mapCount = (data.size() + entriesPerMap - 1) / entriesPerMap;
        if (!allocateSectors(mapCount, maps)) return false;
        size_t dataIndex = 0;
        for (size_t i = 0; i < maps.size(); ++i) {
            vector<uint8_t> sector(sdfsSectorSize, 0);
            put16(&sector[0], i + 1 < maps.size() ? maps[i + 1] : 0);
            put16(&sector[2], i ? maps[i - 1] : 0);
            size_t count = std::min(entriesPerMap, data.size() - dataIndex);
            for (size_t j = 0; j < count; ++j)
                put16(&sector[4 + j * 2], data[dataIndex++]);
            metadata[maps[i]] = sector;
        }
        return true;
    }

    bool allocateFile(File &file, size_t fileIndex) {
        size_t dataCount = (file.length + sdfsSectorSize - 1) / sdfsSectorSize;
        if (!allocateSectors(dataCount, file.dataSectors) ||
            !buildMap(file.dataSectors, file.mapSectors)) return false;
        for (size_t i = 0; i < file.dataSectors.size(); ++i) {
            DataRef &ref = dataRefs[file.dataSectors[i]];
            ref.fileIndex = static_cast<uint32_t>(fileIndex);
            ref.fileSector = i;
            ref.mapped = true;
        }
        return true;
    }

    bool initialize() {
        if (initialized) return imageIsValid;
        initialized = true;
        if (!io || sectors < 64 || sectors > maxSectorCount) return false;
        io->connect();

        struct HostEntry {
            string name;
            size_t length;
        };
        vector<HostEntry> inventory;
        int result = io->readdir("", [&inventory](int, dirent *entry, size_t length) {
            if (!entry || !strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) return;
            HostEntry file = { entry->d_name, length };
            inventory.push_back(file);
        });
        if (result < 0) return false;
        std::sort(inventory.begin(), inventory.end(), [](const HostEntry &left, const HostEntry &right) {
            return atariFilenameLess(left.name, right.name);
        });
        vector<string> hostNames;
        for (size_t i = 0; i < inventory.size(); ++i) hostNames.push_back(inventory[i].name);
        vector<AtariFilename> mappedNames = assignAtariFilenames(hostNames);
        for (size_t i = 0; i < inventory.size(); ++i) {
            if (!mappedNames[i].visible || inventory[i].length > 0xffffff) {
                LOG("SDFS: omitting '%s' (no free alias or unsupported file length)\n", inventory[i].name.c_str());
                continue;
            }
            File file;
            file.hostName = inventory[i].name;
            file.length = inventory[i].length;
            memcpy(file.name, mappedNames[i].name, sizeof(file.name));
            memcpy(file.extension, mappedNames[i].extension, sizeof(file.extension));
            if (mappedNames[i].aliased)
                LOG("SDFS: alias '%s' as '%.8s.%.3s'\n", file.hostName.c_str(), file.name, file.extension);
            files.push_back(file);
        }
        std::sort(files.begin(), files.end(), [](const File &left, const File &right) {
            int order = memcmp(left.name, right.name, sizeof(left.name));
            return order ? order < 0 : memcmp(left.extension, right.extension, sizeof(left.extension)) < 0;
        });

        const size_t entriesPerMap = (sdfsSectorSize - 4) / 2;
        const size_t directoryLength = (files.size() + 1) * directoryRecordSize;
        const size_t directorySectors = (directoryLength + sdfsSectorSize - 1) / sdfsSectorSize;
        const size_t directoryMaps = (directorySectors + entriesPerMap - 1) / entriesPerMap;
        bitmapStart = 4;
        const size_t bitmapBytes = (static_cast<size_t>(sectors) + 8) / 8;
        bitmapSectors = (bitmapBytes + sdfsSectorSize - 1) / sdfsSectorSize;
        if (!bitmapSectors || bitmapSectors > 255 || bitmapStart + bitmapSectors > sectors) return false;

        allocated.assign(sectors + 1, 0);
        dataRefs.assign(sectors + 1, DataRef());
        for (int s = 1; s <= 3; ++s) {
            allocated[s] = 1;
            ++allocatedCount;
        }
        for (int s = bitmapStart; s < bitmapStart + bitmapSectors; ++s) {
            allocated[s] = 1;
            ++allocatedCount;
        }
        allocationSearchStart = bitmapStart + bitmapSectors;

        vector<uint16_t> rootData, rootMaps;
        if (!allocateSectors(directorySectors, rootData) || !buildMap(rootData, rootMaps) ||
            rootMaps.size() != directoryMaps) return false;
        rootMap = rootMaps.empty() ? 0 : rootMaps[0];
        for (size_t i = 0; i < files.size(); ++i)
            if (!allocateFile(files[i], i)) return false;

        vector<uint8_t> directory(directoryLength, 0);
        uint8_t *header = &directory[0];
        put16(&header[1], 0);
        put24(&header[3], directoryLength);
        memcpy(&header[6], "ROOT    ", 8);
        header[17] = 1;
        header[18] = 1;
        for (size_t i = 0; i < files.size(); ++i) {
            uint8_t *entry = &directory[(i + 1) * directoryRecordSize];
            entry[0] = 0x08;
            put16(&entry[1], files[i].mapSectors.empty() ? 0 : files[i].mapSectors[0]);
            put24(&entry[3], files[i].length);
            memcpy(&entry[6], files[i].name, sizeof(files[i].name));
            memcpy(&entry[14], files[i].extension, sizeof(files[i].extension));
            entry[17] = 1;
            entry[18] = 1;
        }
        for (size_t i = 0; i < rootData.size(); ++i) {
            vector<uint8_t> data(sdfsSectorSize, 0);
            size_t offset = i * sdfsSectorSize;
            size_t count = std::min(static_cast<size_t>(sdfsSectorSize), directory.size() - offset);
            memcpy(data.data(), directory.data() + offset, count);
            metadata[rootData[i]] = data;
        }

        bitmap.assign(static_cast<size_t>(bitmapSectors) * sdfsSectorSize, 0xff);
        for (int s = 0; s <= sectors; ++s) {
            if (s == 0 || s > sectors || allocated[s])
                bitmap[s / 8] &= static_cast<uint8_t>(~(0x80 >> (s & 7)));
        }

        vector<uint8_t> boot(128, 0);
        boot[0] = 0;
        boot[1] = 0;
        boot[6] = 0x4c;
        boot[7] = 0x80;
        boot[8] = 0x30;
        put16(&boot[9], rootMap);
        put16(&boot[11], sectors);
        put16(&boot[13], sectors - allocatedCount);
        boot[15] = bitmapSectors;
        put16(&boot[16], bitmapStart);
        put16(&boot[18], allocationSearchStart <= static_cast<uint32_t>(sectors) ? allocationSearchStart : 1);
        put16(&boot[20], allocationSearchStart <= static_cast<uint32_t>(sectors) ? allocationSearchStart : 1);
        memcpy(&boot[22], volumeName.data(), 8);
        boot[30] = 1;
        boot[31] = 0;
        boot[32] = 0x21;
        put16(&boot[35], entriesPerMap);
        boot[37] = 1;
        boot[39] = 0x5a;
        metadata[1] = boot;
        LOG("SDFS init: sectors=%d bitmap=%d@%u rootMap=%u allocEnd=%u files=%zu rootData=%zu rootMaps=%zu\n",
            sectors, bitmapSectors, bitmapStart, rootMap, allocationSearchStart,
            files.size(), rootData.size(), rootMaps.size());
        imageIsValid = true;
        return true;
    }

public:
    explicit DiskStitchImageSDFS(StorageInterface *storage, int sectorCount = maxSectorCount,
                                 const char *label = "DSTITCH")
        : io(storage), sectors(sectorCount), bitmapSectors(0), bitmapStart(0), rootMap(0),
          allocationSearchStart(0), allocatedCount(0), initialized(false), imageIsValid(false),
          volumeName(label ? label : "DSTITCH") {
        if (volumeName.size() > 8) volumeName.resize(8);
        volumeName.append(8 - volumeName.size(), ' ');
        initialize();
    }

    size_t read(uint8_t *buf, size_t sector) override {
        LOG("SDFS read sector=%zu\n", sector);
        if (!initialize() || sector == 0 || sector > static_cast<size_t>(sectors))
            return static_cast<size_t>(-EIO);
        int size = specificSectorSize(static_cast<int>(sector));
        memset(buf, 0, size);
        if (sector >= bitmapStart && sector < bitmapStart + bitmapSectors) {
            size_t offset = (sector - bitmapStart) * sdfsSectorSize;
            memcpy(buf, bitmap.data() + offset, sdfsSectorSize);
            return size;
        }
        map<uint16_t, vector<uint8_t> >::const_iterator found = metadata.find(sector);
        if (found != metadata.end()) {
            memcpy(buf, found->second.data(), std::min<size_t>(size, found->second.size()));
            if (sector == rootMap)
                LOG("SDFS root map bytes=%02x %02x %02x %02x firstEntry=%02x %02x\n",
                    buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
            if (sector == 1)
                LOG("SDFS sector1 jump=%02x %02x %02x root=%u total=%u free=%u bitmapCount=%u bitmapStart=%u search=%u label='%.8s'\n",
                    buf[6], buf[7], buf[8],
                    buf[9] | (buf[10] << 8), buf[11] | (buf[12] << 8),
                    buf[13] | (buf[14] << 8), buf[15], buf[16] | (buf[17] << 8),
                    buf[18] | (buf[19] << 8), buf + 22);
            return size;
        }
        const DataRef &ref = dataRefs[sector];
        if (ref.mapped) {
            const File &file = files[ref.fileIndex];
            size_t offset = static_cast<size_t>(ref.fileSector) * sdfsSectorSize;
            if (offset < file.length) {
                size_t length = std::min(static_cast<size_t>(sdfsSectorSize), file.length - offset);
                if (io->open(file.hostName.c_str(), O_RDONLY) < 0)
                    return static_cast<size_t>(-EIO);
                int count = io->pread(buf, length, offset);
                io->close();
                if (count < 0 || static_cast<size_t>(count) != length)
                    return static_cast<size_t>(-EIO);
            }
        }
        return size;
    }

    int write(const uint8_t *, size_t sector) override {
        LOG("SDFS write-protected sector=%zu\n", sector);
        return -EROFS;
    }
    int sectorSize() override { return sdfsSectorSize; }
    int sectorCount() override { return sectors; }
    bool valid() override { return initialize(); }
};
