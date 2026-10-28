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

    struct TraceCreateIntent {
        string name;
        uint16_t mapSector;
        uint32_t length;
    };

    struct PendingCreate {
        string name;
        uint16_t mapSector;
        uint32_t length;
        vector<uint16_t> dataSectors;
        PendingCreate() : mapSector(0), length(0) {}
    };

    struct PendingAppend {
        size_t fileIndex;
        string name;
        uint16_t mapSector;
        uint32_t oldLength;
        uint32_t newLength;
        uint16_t tailSector;
        uint16_t tailOffset;
        vector<uint16_t> oldDataSectors;
        vector<uint16_t> oldMapSectors;
        vector<uint16_t> dataSectors;
        PendingAppend() : fileIndex(0), mapSector(0), oldLength(0), newLength(0),
                          tailSector(0), tailOffset(0) {}
    };

    struct PendingReplacement {
        size_t fileIndex;
        string name;
        uint16_t mapSector;
        bool warned;
        PendingReplacement() : fileIndex(0), mapSector(0), warned(false) {}
    };

    struct PendingSectorWrite {
        vector<uint8_t> bytes;
        uint64_t firstWrite;
        PendingSectorWrite() : firstWrite(0) {}
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
    map<uint16_t, PendingSectorWrite> pendingSectorWrites;
    uint64_t pendingSectorWriteSequence;
    map<uint16_t, PendingCreate> pendingCreates;
    map<uint16_t, PendingAppend> pendingAppends;
    map<uint16_t, PendingReplacement> pendingReplacements;
    map<uint16_t, vector<uint16_t> > tracePendingData;
    vector<uint16_t> rootDirectorySectors;
    bool traceOverlayEnabled;
    bool writePocEnabled;
    bool traceInterleaveDetected;
    bool traceDataOrderDetected;
    uint64_t traceWriteCount;

    vector<uint16_t> pendingSectorOrder() const {
        vector<std::pair<uint64_t, uint16_t> > ordered;
        for (map<uint16_t, PendingSectorWrite>::const_iterator i = pendingSectorWrites.begin();
             i != pendingSectorWrites.end(); ++i)
            ordered.push_back(std::make_pair(i->second.firstWrite, i->first));
        std::sort(ordered.begin(), ordered.end());
        vector<uint16_t> result;
        result.reserve(ordered.size());
        for (size_t i = 0; i < ordered.size(); ++i) result.push_back(ordered[i].second);
        return result;
    }

    void stageSectorWrite(uint16_t sector, const vector<uint8_t> &bytes) {
        PendingSectorWrite &pending = pendingSectorWrites[sector];
        if (!pending.firstWrite) pending.firstWrite = ++pendingSectorWriteSequence;
        pending.bytes = bytes;
    }

    const char *sectorKind(size_t sector) const {
        if (sector >= bitmapStart && sector < bitmapStart + bitmapSectors) return "bitmap";
        if (metadata.count(static_cast<uint16_t>(sector))) return "generated-metadata";
        if (sector < dataRefs.size() && dataRefs[sector].mapped) return "inventoried-file-data";
        return "unallocated";
    }

    static uint32_t get24(const uint8_t *src) {
        return static_cast<uint32_t>(src[0]) |
               (static_cast<uint32_t>(src[1]) << 8) |
               (static_cast<uint32_t>(src[2]) << 16);
    }

    static uint16_t get16(const uint8_t *src) {
        return static_cast<uint16_t>(src[0] | (src[1] << 8));
    }

    vector<TraceCreateIntent> traceCreateIntents() {
        vector<TraceCreateIntent> intents;
        if (rootDirectorySectors.empty()) return intents;
        vector<uint8_t> directory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        for (size_t i = 0; i < rootDirectorySectors.size(); ++i) {
            uint8_t sector[sdfsSectorSize];
            if (static_cast<int>(read(sector, rootDirectorySectors[i])) < 0) return intents;
            memcpy(directory.data() + i * sdfsSectorSize, sector, sdfsSectorSize);
        }
        size_t length = std::min(static_cast<size_t>(get24(&directory[3])), directory.size());
        for (size_t offset = directoryRecordSize; offset + directoryRecordSize <= length;
             offset += directoryRecordSize) {
            const uint8_t *entry = directory.data() + offset;
            if ((entry[0] & 0x80) == 0) continue;
            TraceCreateIntent intent;
            intent.mapSector = static_cast<uint16_t>(entry[1] | (entry[2] << 8));
            intent.length = get24(entry + 3);
            char name[13];
            size_t nameLength = 8;
            while (nameLength && entry[6 + nameLength - 1] == ' ') --nameLength;
            size_t extensionLength = 3;
            while (extensionLength && entry[14 + extensionLength - 1] == ' ') --extensionLength;
            memcpy(name, entry + 6, nameLength);
            size_t written = nameLength;
            if (extensionLength) {
                name[written++] = '.';
                memcpy(name + written, entry + 14, extensionLength);
                written += extensionLength;
            }
            name[written] = '\0';
            intent.name = name;
            intents.push_back(intent);
        }
        return intents;
    }

    void checkTraceInterleaving(const vector<TraceCreateIntent> &intents) {
        if (intents.size() <= 1 || traceInterleaveDetected) return;
        traceInterleaveDetected = true;
        fprintf(stderr, "SDFS HYPOTHESIS WARNING: overlapping in-progress files:");
        for (size_t i = 0; i < intents.size(); ++i)
            fprintf(stderr, " %s", intents[i].name.c_str());
        fprintf(stderr, "\n");
    }

    void checkTraceDataOrder(uint16_t mapSector, const uint8_t *mapData) {
        vector<uint16_t> &observed = tracePendingData[mapSector];
        size_t listed = 0;
        while (listed < (sdfsSectorSize - 4) / 2 &&
               (mapData[4 + listed * 2] || mapData[5 + listed * 2])) {
            uint16_t sector = static_cast<uint16_t>(mapData[4 + listed * 2] |
                                                    (mapData[5 + listed * 2] << 8));
            if (listed >= observed.size() || observed[listed] != sector) {
                if (!traceDataOrderDetected) {
                    traceDataOrderDetected = true;
                    fprintf(stderr, "SDFS HYPOTHESIS WARNING: map sector %u orders data sector %u at index %zu, observed ",
                            mapSector, sector, listed);
                    if (listed < observed.size()) fprintf(stderr, "%u", observed[listed]);
                    else fprintf(stderr, "no write");
                    fprintf(stderr, "\n");
                }
                return;
            }
            ++listed;
        }
    }

    void checkPendingDirectory() {
        if (pendingCreates.empty() || rootDirectorySectors.empty()) return;
        vector<uint8_t> directory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        for (size_t i = 0; i < rootDirectorySectors.size(); ++i) {
            uint8_t sector[sdfsSectorSize];
            if (static_cast<int>(read(sector, rootDirectorySectors[i])) < 0) continue;
            memcpy(directory.data() + i * sdfsSectorSize, sector, sdfsSectorSize);
        }
        size_t length = std::min(static_cast<size_t>(get24(&directory[3])), directory.size());
        for (map<uint16_t, PendingCreate>::iterator pending = pendingCreates.begin();
             pending != pendingCreates.end(); ++pending) {
            bool found = false;
            for (size_t offset = directoryRecordSize; offset + directoryRecordSize <= length;
                 offset += directoryRecordSize) {
                const uint8_t *entry = directory.data() + offset;
                uint16_t entryMap = static_cast<uint16_t>(entry[1] | (entry[2] << 8));
                if (entryMap != pending->first) continue;
                char entryName[13];
                size_t baseLength = 8;
                while (baseLength && entry[6 + baseLength - 1] == ' ') --baseLength;
                size_t extensionLength = 3;
                while (extensionLength && entry[14 + extensionLength - 1] == ' ') --extensionLength;
                memcpy(entryName, entry + 6, baseLength);
                size_t written = baseLength;
                if (extensionLength) {
                    entryName[written++] = '.';
                    memcpy(entryName + written, entry + 14, extensionLength);
                    written += extensionLength;
                }
                entryName[written] = '\0';
                uint32_t entryLength = get24(entry + 3);
                size_t requiredSectors = (entryLength + sdfsSectorSize - 1) / sdfsSectorSize;
                if (strcmp(entryName, pending->second.name.c_str()) != 0 ||
                    (entry[0] != 0x88 && entry[0] != 0x08) ||
                    (entry[0] == 0x08 && requiredSectors != pending->second.dataSectors.size())) {
                    traceDataOrderDetected = true;
                    fprintf(stderr, "SDFS HYPOTHESIS WARNING: dirent for %s contradicts pending data-sector hypothesis\n",
                            pending->second.name.c_str());
                }
                found = true;
                break;
            }
            if (!found) {
                traceDataOrderDetected = true;
                fprintf(stderr, "SDFS HYPOTHESIS WARNING: pending file %s disappeared from root dirent data\n",
                        pending->second.name.c_str());
            }
        }
    }

    bool isInventoriedName(const string &name) const {
        size_t dot = name.find('.');
        size_t baseLength = dot == string::npos ? name.size() : dot;
        size_t extensionLength = dot == string::npos ? 0 : name.size() - dot - 1;
        if (!baseLength || baseLength > 8 || extensionLength > 3) return false;
        char base[8];
        char extension[3];
        memset(base, ' ', sizeof(base));
        memset(extension, ' ', sizeof(extension));
        memcpy(base, name.data(), baseLength);
        if (extensionLength) memcpy(extension, name.data() + dot + 1, extensionLength);
        for (size_t i = 0; i < files.size(); ++i)
            if (files[i].length && !memcmp(base, files[i].name, sizeof(base)) &&
                !memcmp(extension, files[i].extension, sizeof(extension))) return true;
        return false;
    }

    bool finalizedDirectoryEntry(uint16_t mapSector, const string &name, uint32_t &length) {
        if (rootDirectorySectors.empty()) return false;
        vector<uint8_t> directory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        for (size_t i = 0; i < rootDirectorySectors.size(); ++i) {
            uint8_t sector[sdfsSectorSize];
            if (static_cast<int>(read(sector, rootDirectorySectors[i])) < 0) return false;
            memcpy(directory.data() + i * sdfsSectorSize, sector, sdfsSectorSize);
        }
        size_t directoryLength = std::min(static_cast<size_t>(get24(&directory[3])), directory.size());
        for (size_t offset = directoryRecordSize; offset + directoryRecordSize <= directoryLength;
             offset += directoryRecordSize) {
            const uint8_t *entry = directory.data() + offset;
            if (entry[0] != 0x08 ||
                static_cast<uint16_t>(entry[1] | (entry[2] << 8)) != mapSector) continue;
            char entryName[13];
            size_t nameLength = 8;
            while (nameLength && entry[6 + nameLength - 1] == ' ') --nameLength;
            size_t extensionLength = 3;
            while (extensionLength && entry[14 + extensionLength - 1] == ' ') --extensionLength;
            memcpy(entryName, entry + 6, nameLength);
            size_t written = nameLength;
            if (extensionLength) {
                entryName[written++] = '.';
                memcpy(entryName + written, entry + 14, extensionLength);
                written += extensionLength;
            }
            entryName[written] = '\0';
            if (name == entryName) {
                length = get24(entry + 3);
                return true;
            }
        }
        return false;
    }

    void beginPendingReplacement(size_t directorySector, const uint8_t *beforeSector,
                                 const uint8_t *afterSector) {
        vector<uint8_t> beforeDirectory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        vector<uint8_t> afterDirectory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        for (size_t i = 0; i < rootDirectorySectors.size(); ++i) {
            uint8_t current[sdfsSectorSize];
            if (rootDirectorySectors[i] == directorySector) {
                memcpy(beforeDirectory.data() + i * sdfsSectorSize, beforeSector, sdfsSectorSize);
                memcpy(afterDirectory.data() + i * sdfsSectorSize, afterSector, sdfsSectorSize);
            } else {
                if (static_cast<int>(read(current, rootDirectorySectors[i])) < 0) return;
                memcpy(beforeDirectory.data() + i * sdfsSectorSize, current, sdfsSectorSize);
                memcpy(afterDirectory.data() + i * sdfsSectorSize, current, sdfsSectorSize);
            }
        }
        size_t directoryLength = std::min(static_cast<size_t>(get24(&afterDirectory[3])),
                                          afterDirectory.size());
        for (size_t offset = directoryRecordSize; offset + directoryRecordSize <= directoryLength;
             offset += directoryRecordSize) {
            const uint8_t *before = beforeDirectory.data() + offset;
            const uint8_t *after = afterDirectory.data() + offset;
            if (before[0] != 0x08 || after[0] != 0x88) continue;
            uint16_t firstMap = get16(before + 1);
            string name = visibleEntryName(before);
            for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
                File &file = files[fileIndex];
                if (file.mapSectors.empty() || file.mapSectors[0] != firstMap ||
                    pendingVisibleName(fileIndex) != name) continue;
                PendingReplacement pending;
                pending.fileIndex = fileIndex;
                pending.name = file.hostName;
                pending.mapSector = firstMap;
                pendingReplacements[firstMap] = pending;
                LOG("SDFS write POC: waiting for '%s' dirent to become active\n", name.c_str());
                break;
            }
        }
    }

    int commitPendingReplacement(PendingReplacement &pending) {
        if (pending.fileIndex >= files.size()) return -EROFS;
        File &file = files[pending.fileIndex];
        if (file.hostName != pending.name || file.mapSectors.empty() ||
            file.mapSectors[0] != pending.mapSector) return -EROFS;
        uint32_t newLength = 0;
        string name = pendingVisibleName(pending.fileIndex);
        if (!finalizedDirectoryEntry(pending.mapSector, name, newLength)) return 0;

        vector<uint16_t> maps;
        vector<uint16_t> listed;
        map<uint16_t, vector<uint8_t> > mapImages;
        bool incomplete = false;
        if (!readMapChain(pending.mapSector, file.mapSectors, maps, listed, mapImages, incomplete))
            return -EROFS;
        if (incomplete) return 0;
        size_t requiredSectors = (newLength + sdfsSectorSize - 1) / sdfsSectorSize;
        if (listed.size() < requiredSectors) return 0;
        if (listed.size() != requiredSectors || maps.empty()) return -EROFS;

        vector<uint8_t> contents;
        contents.reserve(newLength);
        for (size_t i = 0; i < listed.size(); ++i) {
            map<uint16_t, PendingSectorWrite>::const_iterator queued = pendingSectorWrites.find(listed[i]);
            if (queued == pendingSectorWrites.end() || queued->second.bytes.size() != sdfsSectorSize) return 0;
            size_t count = std::min(static_cast<size_t>(sdfsSectorSize),
                                    static_cast<size_t>(newLength) - contents.size());
            contents.insert(contents.end(), queued->second.bytes.begin(),
                            queued->second.bytes.begin() + count);
        }
        if (contents.size() != newLength) return -EROFS;

        for (size_t i = 0; i < listed.size(); ++i) {
            uint16_t sector = listed[i];
            if (sector >= dataRefs.size() ||
                (dataRefs[sector].mapped && dataRefs[sector].fileIndex != pending.fileIndex)) {
                if (!pending.warned) {
                    fprintf(stderr, "SDFS HYPOTHESIS WARNING: replacement '%s' claims data sector %u owned by another file; leaving it pending\n",
                            name.c_str(), sector);
                    pending.warned = true;
                }
                return 0;
            }
        }
        for (size_t i = 0; i < maps.size(); ++i) {
            uint16_t sector = maps[i];
            if (sector >= allocated.size() ||
                (dataRefs[sector].mapped && dataRefs[sector].fileIndex != pending.fileIndex)) {
                if (!pending.warned) {
                    fprintf(stderr, "SDFS HYPOTHESIS WARNING: replacement '%s' claims map sector %u owned by another file; leaving it pending\n",
                            name.c_str(), sector);
                    pending.warned = true;
                }
                return 0;
            }
        }

        vector<uint8_t> oldContents(file.length);
        if (io->open(file.hostName.c_str(), O_RDONLY) < 0) return -EIO;
        size_t oldOffset = 0;
        while (oldOffset < oldContents.size()) {
            int count = io->pread(oldContents.data() + oldOffset, oldContents.size() - oldOffset, oldOffset);
            if (count <= 0) break;
            oldOffset += static_cast<size_t>(count);
        }
        io->close();
        if (oldOffset != oldContents.size()) return -EIO;

        if (io->truncate(file.hostName.c_str(), 0) < 0 || io->open(file.hostName.c_str(), O_RDWR) < 0)
            return -EIO;
        int written = newLength ? io->pwrite(contents.data(), contents.size(), 0, false) : 0;
        io->close();
        if (written < 0 || static_cast<size_t>(written) != contents.size()) {
            io->truncate(file.hostName.c_str(), 0);
            if (io->open(file.hostName.c_str(), O_RDWR) >= 0) {
                if (!oldContents.empty()) io->pwrite(oldContents.data(), oldContents.size(), 0, false);
                io->close();
                io->truncate(file.hostName.c_str(), oldContents.size());
            }
            return -EIO;
        }

        vector<uint16_t> oldData = file.dataSectors;
        vector<uint16_t> oldMaps = file.mapSectors;
        for (size_t i = 0; i < oldData.size(); ++i) {
            uint16_t sector = oldData[i];
            if (std::find(listed.begin(), listed.end(), sector) != listed.end()) continue;
            if (sector < dataRefs.size()) dataRefs[sector] = DataRef();
            if (sector < allocated.size() && allocated[sector]) {
                allocated[sector] = 0;
                if (allocatedCount) --allocatedCount;
            }
        }
        for (size_t i = 0; i < oldMaps.size(); ++i) {
            uint16_t sector = oldMaps[i];
            if (std::find(maps.begin(), maps.end(), sector) != maps.end()) continue;
            metadata.erase(sector);
            if (sector < allocated.size() && allocated[sector]) {
                allocated[sector] = 0;
                if (allocatedCount) --allocatedCount;
            }
        }
        for (size_t i = 0; i < listed.size(); ++i) {
            uint16_t sector = listed[i];
            if (!allocated[sector]) {
                allocated[sector] = 1;
                ++allocatedCount;
            }
            dataRefs[sector].fileIndex = static_cast<uint32_t>(pending.fileIndex);
            dataRefs[sector].fileSector = i;
            dataRefs[sector].mapped = true;
        }
        for (size_t i = 0; i < maps.size(); ++i) {
            uint16_t sector = maps[i];
            if (!allocated[sector]) {
                allocated[sector] = 1;
                ++allocatedCount;
            }
            metadata[sector] = mapImages[sector];
        }
        file.length = newLength;
        file.dataSectors = listed;
        file.mapSectors = maps;
        vector<uint16_t> resolved = oldData;
        resolved.insert(resolved.end(), oldMaps.begin(), oldMaps.end());
        resolved.insert(resolved.end(), listed.begin(), listed.end());
        resolved.insert(resolved.end(), maps.begin(), maps.end());
        for (size_t i = 0; i < resolved.size(); ++i) {
            pendingSectorWrites.erase(resolved[i]);
        }
        LOG("SDFS write POC: replaced '%s' length=%u data-sectors=%zu map-sectors=%zu\n",
            file.hostName.c_str(), file.length, file.dataSectors.size(), file.mapSectors.size());
        return 1;
    }

    bool readMapChain(uint16_t firstMap, const vector<uint16_t> &oldMaps,
                      vector<uint16_t> &maps, vector<uint16_t> &dataSectors,
                      map<uint16_t, vector<uint8_t> > &mapImages, bool &incomplete) {
        maps.clear();
        dataSectors.clear();
        mapImages.clear();
        incomplete = false;
        uint16_t current = firstMap;
        uint16_t previous = 0;
        while (current) {
            if (std::find(maps.begin(), maps.end(), current) != maps.end()) return false;
            if (maps.size() > static_cast<size_t>(sectors)) return false;
            if (maps.size() >= oldMaps.size() &&
                !metadata.count(current) && !pendingSectorWrites.count(current)) {
                incomplete = true;
                return true;
            }
            uint8_t bytes[sdfsSectorSize];
            if (static_cast<int>(read(bytes, current)) < 0) return false;
            if (maps.size() && get16(bytes + 2) != previous) return false;
            vector<uint8_t> image(bytes, bytes + sdfsSectorSize);
            mapImages[current] = image;
            maps.push_back(current);
            for (size_t i = 0; i < (sdfsSectorSize - 4) / 2; ++i) {
                uint16_t dataSector = get16(bytes + 4 + i * 2);
                if (!dataSector) break;
                dataSectors.push_back(dataSector);
            }
            previous = current;
            current = get16(bytes);
        }
        return true;
    }

    int commitPendingCreate(PendingCreate &pending) {
        uint32_t length = 0;
        if (!finalizedDirectoryEntry(pending.mapSector, pending.name, length)) return 0;
        if (length > 0xffffff || length != pending.length || isInventoriedName(pending.name)) return -EROFS;
        vector<uint16_t> maps;
        vector<uint16_t> listed;
        map<uint16_t, vector<uint8_t> > mapImages;
        bool incomplete = false;
        if (!readMapChain(pending.mapSector, vector<uint16_t>(), maps, listed,
                          mapImages, incomplete)) return -EROFS;
        if (incomplete) return 0;
        size_t requiredSectors = (length + sdfsSectorSize - 1) / sdfsSectorSize;
        if (listed.size() < requiredSectors) return 0;
        if (listed.size() != requiredSectors)
            return -EROFS;
        if (maps.empty() || maps.size() > requiredSectors + 1) return -EROFS;
        vector<uint16_t> observed;
        vector<uint16_t> writeOrder = pendingSectorOrder();
        for (size_t i = 0; i < writeOrder.size(); ++i) {
            uint16_t sector = writeOrder[i];
            if (std::find(listed.begin(), listed.end(), sector) == listed.end() ||
                std::find(maps.begin(), maps.end(), sector) != maps.end()) continue;
            observed.push_back(sector);
        }
        if (observed.size() < listed.size()) return 0;
        if (observed != listed) {
            traceDataOrderDetected = true;
            fprintf(stderr, "SDFS HYPOTHESIS WARNING: create payload order differs from map order for '%s'; using map order\n",
                    pending.name.c_str());
        }
        pending.dataSectors = observed;
        for (size_t i = 0; i < listed.size(); ++i) {
            if (!pendingSectorWrites.count(listed[i])) return 0;
        }

        size_t dot = pending.name.find('.');
        size_t baseLength = dot == string::npos ? pending.name.size() : dot;
        size_t extensionLength = dot == string::npos ? 0 : pending.name.size() - dot - 1;
        if (!baseLength || baseLength > 8 || extensionLength > 3) return -EROFS;
        string hostName = pending.name;
        if (io->open(hostName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644) < 0) return -EROFS;
        size_t offset = 0;
        bool writeFailed = false;
        for (size_t i = 0; i < listed.size(); ++i) {
            size_t count = std::min(static_cast<size_t>(sdfsSectorSize), static_cast<size_t>(length) - offset);
            const vector<uint8_t> &sectorData = pendingSectorWrites[listed[i]].bytes;
            int written = io->pwrite(sectorData.data(), count, offset, false);
            if (written < 0 || static_cast<size_t>(written) != count) {
                writeFailed = true;
                break;
            }
            offset += count;
        }
        io->close();
        if (writeFailed) {
            io->remove(hostName.c_str());
            return -EIO;
        }

        File file;
        file.hostName = hostName;
        file.length = length;
        memset(file.name, ' ', sizeof(file.name));
        memset(file.extension, ' ', sizeof(file.extension));
        memcpy(file.name, pending.name.data(), baseLength);
        if (extensionLength) memcpy(file.extension, pending.name.data() + dot + 1, extensionLength);
        file.dataSectors = listed;
        file.mapSectors = maps;
        size_t fileIndex = files.size();
        files.push_back(file);
        for (size_t i = 0; i < listed.size(); ++i) {
            allocated[listed[i]] = 1;
            dataRefs[listed[i]].fileIndex = static_cast<uint32_t>(fileIndex);
            dataRefs[listed[i]].fileSector = i;
            dataRefs[listed[i]].mapped = true;
            ++allocatedCount;
        }
        for (size_t i = 0; i < maps.size(); ++i) {
            allocated[maps[i]] = 1;
            ++allocatedCount;
            metadata[maps[i]] = mapImages[maps[i]];
        }

        vector<uint16_t> committedSectors;
        for (map<uint16_t, PendingSectorWrite>::iterator overlay = pendingSectorWrites.begin();
             overlay != pendingSectorWrites.end(); ++overlay) {
            uint16_t sector = overlay->first;
            if (sector >= bitmapStart && sector < bitmapStart + bitmapSectors) {
                size_t offset = (sector - bitmapStart) * sdfsSectorSize;
                memcpy(bitmap.data() + offset, overlay->second.bytes.data(), sdfsSectorSize);
                committedSectors.push_back(sector);
            } else if (metadata.count(sector)) {
                metadata[sector] = overlay->second.bytes;
                committedSectors.push_back(sector);
            }
        }
        committedSectors.insert(committedSectors.end(), maps.begin(), maps.end());
        committedSectors.insert(committedSectors.end(), pending.dataSectors.begin(), pending.dataSectors.end());
        for (size_t i = 0; i < committedSectors.size(); ++i)
            pendingSectorWrites.erase(committedSectors[i]);
        for (size_t i = 0; i < maps.size(); ++i)
            pendingSectorWrites.erase(maps[i]);
        for (size_t i = 0; i < pending.dataSectors.size(); ++i)
            pendingSectorWrites.erase(pending.dataSectors[i]);
        return 1;
    }

    string visibleEntryName(const uint8_t *entry) const {
        char name[13];
        size_t baseLength = 8;
        while (baseLength && entry[6 + baseLength - 1] == ' ') --baseLength;
        size_t extensionLength = 3;
        while (extensionLength && entry[14 + extensionLength - 1] == ' ') --extensionLength;
        memcpy(name, entry + 6, baseLength);
        size_t written = baseLength;
        if (extensionLength) {
            name[written++] = '.';
            memcpy(name + written, entry + 14, extensionLength);
            written += extensionLength;
        }
        name[written] = '\0';
        return string(name);
    }

    int commitRenamedDirectoryEntries(size_t directorySector,
                                      const uint8_t *beforeSector,
                                      const uint8_t *afterSector) {
        vector<uint8_t> beforeDirectory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        vector<uint8_t> afterDirectory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        for (size_t i = 0; i < rootDirectorySectors.size(); ++i) {
            uint8_t current[sdfsSectorSize];
            if (rootDirectorySectors[i] == directorySector) {
                memcpy(beforeDirectory.data() + i * sdfsSectorSize, beforeSector, sdfsSectorSize);
                memcpy(afterDirectory.data() + i * sdfsSectorSize, afterSector, sdfsSectorSize);
            } else {
                if (static_cast<int>(read(current, rootDirectorySectors[i])) < 0) return -EIO;
                memcpy(beforeDirectory.data() + i * sdfsSectorSize, current, sdfsSectorSize);
                memcpy(afterDirectory.data() + i * sdfsSectorSize, current, sdfsSectorSize);
            }
        }
        size_t directoryLength = std::min(static_cast<size_t>(get24(&afterDirectory[3])),
                                          afterDirectory.size());
        for (size_t offset = directoryRecordSize; offset + directoryRecordSize <= directoryLength;
             offset += directoryRecordSize) {
            const uint8_t *before = beforeDirectory.data() + offset;
            const uint8_t *after = afterDirectory.data() + offset;
            bool sameName = !memcmp(before + 6, after + 6, 8) &&
                            !memcmp(before + 14, after + 14, 3);
            if (before[0] != 0x08 || after[0] != 0x08 ||
                get16(before + 1) != get16(after + 1) ||
                get24(before + 3) != get24(after + 3) || sameName) continue;

            uint16_t firstMap = get16(before + 1);
            string oldVisibleName = visibleEntryName(before);
            string newVisibleName = visibleEntryName(after);
            size_t fileIndex = files.size();
            for (size_t i = 0; i < files.size(); ++i) {
                if (!files[i].mapSectors.empty() && files[i].mapSectors[0] == firstMap &&
                    pendingVisibleName(i) == oldVisibleName) {
                    fileIndex = i;
                    break;
                }
            }
            if (fileIndex == files.size()) continue;

            for (size_t i = 0; i < files.size(); ++i) {
                if (i != fileIndex && files[i].length &&
                    pendingVisibleName(i) == newVisibleName) return -EEXIST;
            }

            File &file = files[fileIndex];
            if (file.hostName != newVisibleName) {
                int result = io->rename(file.hostName.c_str(), newVisibleName.c_str());
                if (result < 0) return result;
            }
            file.hostName = newVisibleName;
            memcpy(file.name, after + 6, sizeof(file.name));
            memcpy(file.extension, after + 14, sizeof(file.extension));
            pendingReplacements.erase(firstMap);
            LOG("SDFS write POC: renamed '%s' to '%s'\n",
                oldVisibleName.c_str(), newVisibleName.c_str());
        }
        return 0;
    }

    bool stageHypothesisFromDirectoryUpdate(size_t directorySector,
                                           const uint8_t *beforeSector,
                                           const uint8_t *afterSector) {
        if (!writePocEnabled) return true;
        vector<uint8_t> beforeDirectory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        vector<uint8_t> afterDirectory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        for (size_t i = 0; i < rootDirectorySectors.size(); ++i) {
            uint8_t data[sdfsSectorSize];
            if (rootDirectorySectors[i] == directorySector) {
                memcpy(data, beforeSector, sdfsSectorSize);
                memcpy(beforeDirectory.data() + i * sdfsSectorSize, data, sdfsSectorSize);
                memcpy(data, afterSector, sdfsSectorSize);
                memcpy(afterDirectory.data() + i * sdfsSectorSize, data, sdfsSectorSize);
            } else {
                if (static_cast<int>(read(data, rootDirectorySectors[i])) < 0) return false;
                memcpy(beforeDirectory.data() + i * sdfsSectorSize, data, sdfsSectorSize);
                memcpy(afterDirectory.data() + i * sdfsSectorSize, data, sdfsSectorSize);
            }
        }
        size_t directoryLength = std::min(static_cast<size_t>(get24(&afterDirectory[3])),
                                          afterDirectory.size());
        for (size_t offset = directoryRecordSize; offset + directoryRecordSize <= directoryLength;
             offset += directoryRecordSize) {
            const uint8_t *before = beforeDirectory.data() + offset;
            const uint8_t *after = afterDirectory.data() + offset;
            if (!memcmp(before, after, directoryRecordSize) || after[0] != 0x08) continue;

            uint16_t mapSector = get16(after + 1);
            uint32_t newLength = get24(after + 3);
            string visibleName = visibleEntryName(after);
            size_t fileIndex = files.size();
            for (size_t i = 0; i < files.size(); ++i) {
                if (!files[i].mapSectors.empty() && files[i].mapSectors[0] == mapSector &&
                    pendingVisibleName(i) == visibleName) {
                    fileIndex = i;
                    break;
                }
            }

            if (fileIndex < files.size()) {
                const File &file = files[fileIndex];
                if (newLength <= file.length || pendingAppends.count(mapSector)) continue;
                if (file.mapSectors.empty() || file.dataSectors.empty()) return false;
                PendingAppend pending;
                pending.fileIndex = fileIndex;
                pending.name = file.hostName;
                pending.mapSector = mapSector;
                pending.oldLength = file.length;
                pending.newLength = newLength;
                pending.oldDataSectors = file.dataSectors;
                pending.oldMapSectors = file.mapSectors;
                if (file.length % sdfsSectorSize) {
                    pending.tailSector = file.dataSectors.back();
                    pending.tailOffset = static_cast<uint16_t>(file.length % sdfsSectorSize);
                }
                vector<uint16_t> writeOrder = pendingSectorOrder();
                for (size_t i = 0; i < writeOrder.size(); ++i) {
                    uint16_t sector = writeOrder[i];
                    if (sector == mapSector || sector >= dataRefs.size() || dataRefs[sector].mapped) continue;
                    pending.dataSectors.push_back(sector);
                }
                pendingAppends[mapSector] = pending;
                LOG("SDFS write POC: provisional append for '%s' old=%u new=%u map=%u sectors=%zu\n",
                    pending.name.c_str(), pending.oldLength, pending.newLength, mapSector,
                    pending.dataSectors.size());
                continue;
            }

            if (newLength == 0 || isInventoriedName(visibleName) ||
                !pendingSectorWrites.count(mapSector) || pendingCreates.count(mapSector)) continue;
            PendingCreate pending;
            pending.name = visibleName;
            pending.mapSector = mapSector;
            pending.length = newLength;
            vector<uint16_t> writeOrder = pendingSectorOrder();
            for (size_t i = 0; i < writeOrder.size(); ++i) {
                uint16_t sector = writeOrder[i];
                if (sector == mapSector || sector >= dataRefs.size() || dataRefs[sector].mapped) continue;
                pending.dataSectors.push_back(sector);
            }
            pendingCreates[mapSector] = pending;
            LOG("SDFS write POC: provisional create '%s' length=%u map=%u sectors=%zu\n",
                pending.name.c_str(), pending.length, mapSector, pending.dataSectors.size());
        }
        return true;
    }

    int commitPendingAppend(PendingAppend &pending) {
        if (pending.fileIndex >= files.size()) return -EROFS;
        File &file = files[pending.fileIndex];
        if (file.hostName != pending.name || file.length != pending.oldLength ||
            file.mapSectors != pending.oldMapSectors || file.mapSectors.empty() ||
            file.mapSectors[0] != pending.mapSector || pending.oldDataSectors != file.dataSectors)
            return -EROFS;
        if (pending.newLength <= pending.oldLength || pending.newLength > 0xffffff) return -EROFS;

        size_t oldSectorCount = (pending.oldLength + sdfsSectorSize - 1) / sdfsSectorSize;
        size_t newSectorCount = (pending.newLength + sdfsSectorSize - 1) / sdfsSectorSize;
        size_t requiredNewSectors = newSectorCount - oldSectorCount;
        vector<uint16_t> maps;
        vector<uint16_t> listed;
        map<uint16_t, vector<uint8_t> > mapImages;
        bool incomplete = false;
        if (!readMapChain(pending.mapSector, pending.oldMapSectors, maps, listed,
                          mapImages, incomplete)) return -EROFS;
        if (incomplete || maps.size() < pending.oldMapSectors.size() || listed.size() < newSectorCount)
            return 0;
        if (!std::equal(pending.oldMapSectors.begin(), pending.oldMapSectors.end(), maps.begin()) ||
            !std::equal(pending.oldDataSectors.begin(), pending.oldDataSectors.end(), listed.begin()))
            return -EROFS;
        if (listed.size() != newSectorCount) return -EROFS;

        vector<uint16_t> newMapSectors(maps.begin() + pending.oldMapSectors.size(), maps.end());
        vector<uint16_t> mappedNewData(listed.begin() + pending.oldDataSectors.size(), listed.end());
        for (size_t i = 0; i < mappedNewData.size(); ++i) {
            if (std::find(maps.begin(), maps.end(), mappedNewData[i]) != maps.end()) return -EROFS;
            if (std::find(mappedNewData.begin(), mappedNewData.begin() + i, mappedNewData[i]) !=
                mappedNewData.begin() + i) return -EROFS;
        }
        if (mappedNewData.size() != requiredNewSectors) return -EROFS;

        vector<uint16_t> observedNewSectors;
        vector<uint16_t> writeOrder = pendingSectorOrder();
        for (size_t i = 0; i < writeOrder.size(); ++i) {
            uint16_t sector = writeOrder[i];
            if (std::find(mappedNewData.begin(), mappedNewData.end(), sector) == mappedNewData.end())
                continue;
            observedNewSectors.push_back(sector);
        }
        if (observedNewSectors.size() < requiredNewSectors) return 0;
        if (!std::equal(observedNewSectors.begin(), observedNewSectors.end(),
                        mappedNewData.begin())) {
            traceDataOrderDetected = true;
            fprintf(stderr, "SDFS HYPOTHESIS WARNING: append payload order differs from map order for '%s'; using map order\n",
                    pendingVisibleName(pending.fileIndex).c_str());
        }
        pending.dataSectors = mappedNewData;
        for (size_t i = 0; i < mappedNewData.size(); ++i)
            if (!pendingSectorWrites.count(mappedNewData[i])) return 0;
        uint32_t directoryLength = 0;
        if (!finalizedDirectoryEntry(pending.mapSector, pendingVisibleName(pending.fileIndex), directoryLength))
            return 0;
        if (directoryLength != pending.newLength) return -EROFS;

        bool needsTailData = pending.oldLength % sdfsSectorSize &&
            pending.newLength > pending.oldLength &&
            pending.oldLength % sdfsSectorSize < sdfsSectorSize;
        if (needsTailData) {
            map<uint16_t, PendingSectorWrite>::const_iterator tail = pendingSectorWrites.find(pending.tailSector);
            if (tail == pendingSectorWrites.end()) return 0;
            if (pending.tailSector != pending.oldDataSectors.back() ||
                pending.tailOffset != pending.oldLength % sdfsSectorSize ||
                tail->second.bytes.size() != sdfsSectorSize) return -EROFS;
        }

        if (requiredNewSectors) {
            for (size_t i = 0; i < newMapSectors.size(); ++i) {
                uint16_t sector = newMapSectors[i];
                if (allocated[sector]) return -EROFS;
            }
            for (size_t i = 0; i < pending.dataSectors.size(); ++i) {
                uint16_t sector = pending.dataSectors[i];
                if (allocated[sector] || !pendingSectorWrites.count(sector) ||
                    (i && pending.dataSectors[i - 1] == sector)) return -EROFS;
            }
        }

        if (io->open(file.hostName.c_str(), O_RDWR) < 0) return -EROFS;
        bool writeFailed = false;
        size_t writtenOffset = pending.oldLength;
        if (needsTailData) {
            size_t tailOffset = pending.oldLength - pending.tailOffset;
            size_t tailBytes = std::min(static_cast<size_t>(sdfsSectorSize),
                                        static_cast<size_t>(pending.newLength) - tailOffset);
            const vector<uint8_t> &tailData = pendingSectorWrites[pending.tailSector].bytes;
            int written = io->pwrite(tailData.data(), tailBytes, tailOffset, false);
            if (written < 0 || static_cast<size_t>(written) != tailBytes) writeFailed = true;
            writtenOffset = tailOffset + tailBytes;
        }
        for (size_t i = 0; !writeFailed && i < pending.dataSectors.size(); ++i) {
            const vector<uint8_t> &data = pendingSectorWrites[pending.dataSectors[i]].bytes;
            size_t count = std::min(static_cast<size_t>(sdfsSectorSize),
                                    static_cast<size_t>(pending.newLength) - writtenOffset);
            int written = io->pwrite(data.data(), count, writtenOffset, false);
            if (written < 0 || static_cast<size_t>(written) != count) {
                writeFailed = true;
                break;
            }
            writtenOffset += count;
        }
        io->close();
        if (writeFailed) {
            io->truncate(file.hostName.c_str(), pending.oldLength);
            return -EIO;
        }

        for (size_t i = 0; i < pending.dataSectors.size(); ++i) {
            uint16_t sector = pending.dataSectors[i];
            allocated[sector] = 1;
            dataRefs[sector].fileIndex = static_cast<uint32_t>(pending.fileIndex);
            dataRefs[sector].fileSector = file.dataSectors.size();
            dataRefs[sector].mapped = true;
            file.dataSectors.push_back(sector);
            ++allocatedCount;
        }
        for (size_t i = 0; i < newMapSectors.size(); ++i) {
            allocated[newMapSectors[i]] = 1;
            ++allocatedCount;
        }
        file.length = pending.newLength;
        file.mapSectors = maps;
        for (map<uint16_t, vector<uint8_t> >::const_iterator image = mapImages.begin();
             image != mapImages.end(); ++image)
            metadata[image->first] = image->second;
        vector<uint16_t> committedSectors;
        for (map<uint16_t, PendingSectorWrite>::iterator overlay = pendingSectorWrites.begin();
             overlay != pendingSectorWrites.end(); ++overlay) {
            uint16_t sector = overlay->first;
            if (sector >= bitmapStart && sector < bitmapStart + bitmapSectors) {
                size_t offset = (sector - bitmapStart) * sdfsSectorSize;
                memcpy(bitmap.data() + offset, overlay->second.bytes.data(), sdfsSectorSize);
                committedSectors.push_back(sector);
            } else if (metadata.count(sector)) {
                metadata[sector] = overlay->second.bytes;
                committedSectors.push_back(sector);
            }
        }
        committedSectors.insert(committedSectors.end(), maps.begin(), maps.end());
        committedSectors.insert(committedSectors.end(), pending.dataSectors.begin(), pending.dataSectors.end());
        if (needsTailData) committedSectors.push_back(pending.tailSector);
        for (size_t i = 0; i < committedSectors.size(); ++i)
            pendingSectorWrites.erase(committedSectors[i]);
        vector<uint16_t> resolvedSectors = pending.dataSectors;
        resolvedSectors.insert(resolvedSectors.end(), newMapSectors.begin(), newMapSectors.end());
        if (needsTailData) resolvedSectors.push_back(pending.tailSector);
        for (size_t i = 0; i < resolvedSectors.size(); ++i)
            pendingSectorWrites.erase(resolvedSectors[i]);
        LOG("SDFS write POC: appended %u bytes to '%s', new length=%u\n",
            pending.newLength - pending.oldLength, file.hostName.c_str(), file.length);
        return 1;
    }

    string pendingVisibleName(size_t fileIndex) const {
        string name(files[fileIndex].name, files[fileIndex].name + sizeof(files[fileIndex].name));
        size_t baseLength = name.find_last_not_of(' ');
        if (baseLength == string::npos) return string();
        name.resize(baseLength + 1);
        string extension(files[fileIndex].extension,
                         files[fileIndex].extension + sizeof(files[fileIndex].extension));
        size_t extensionLength = extension.find_last_not_of(' ');
        if (extensionLength != string::npos) {
            extension.resize(extensionLength + 1);
            name += "." + extension;
        }
        return name;
    }

    void traceSectorWrite(const uint8_t *before, const uint8_t *after,
                          size_t length, size_t sector, const char *owner) {
        ++traceWriteCount;
        size_t changed = 0;
        for (size_t i = 0; i < length; ++i)
            if (before[i] != after[i]) ++changed;
        fprintf(stderr, "SDFS TRACE #%llu sector=%zu initial-kind=%s owner=%s changed=%zu/%zu\n",
                static_cast<unsigned long long>(traceWriteCount), sector,
                sectorKind(sector), owner, changed, length);
        for (size_t row = 0; row < length; row += 16) {
            size_t rowEnd = std::min(row + 16, length);
            bool rowChanged = false;
            for (size_t i = row; i < rowEnd; ++i)
                if (before[i] != after[i]) rowChanged = true;
            if (!rowChanged) continue;
            fprintf(stderr, "  %03zu:", row);
            for (size_t i = row; i < rowEnd; ++i) fprintf(stderr, " %02x", before[i]);
            fprintf(stderr, " ->");
            for (size_t i = row; i < rowEnd; ++i) fprintf(stderr, " %02x", after[i]);
            fprintf(stderr, "\n");
        }
    }

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
        rootDirectorySectors = rootData;

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

    int commitDeletedDirectoryEntries(size_t directorySector,
                                      const uint8_t *beforeSector,
                                      const uint8_t *afterSector) {
        vector<uint8_t> beforeDirectory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        vector<uint8_t> afterDirectory(rootDirectorySectors.size() * sdfsSectorSize, 0);
        for (size_t i = 0; i < rootDirectorySectors.size(); ++i) {
            uint8_t current[sdfsSectorSize];
            if (static_cast<int>(read(current, rootDirectorySectors[i])) < 0) return -EIO;
            memcpy(beforeDirectory.data() + i * sdfsSectorSize, current, sdfsSectorSize);
            memcpy(afterDirectory.data() + i * sdfsSectorSize, current, sdfsSectorSize);
            if (rootDirectorySectors[i] == directorySector) {
                memcpy(beforeDirectory.data() + i * sdfsSectorSize, beforeSector, sdfsSectorSize);
                memcpy(afterDirectory.data() + i * sdfsSectorSize, afterSector, sdfsSectorSize);
            }
        }
        size_t directoryLength = std::min(static_cast<size_t>(get24(&afterDirectory[3])),
                                          afterDirectory.size());
        bool deletedAny = false;
        for (size_t offset = directoryRecordSize; offset + directoryRecordSize <= directoryLength;
             offset += directoryRecordSize) {
            const uint8_t *before = beforeDirectory.data() + offset;
            const uint8_t *after = afterDirectory.data() + offset;
            if (before[0] != 0x08 || after[0] != 0x10) continue;
            uint16_t firstMap = get16(before + 1);
            string name = visibleEntryName(before);
            size_t fileIndex = files.size();
            for (size_t i = 0; i < files.size(); ++i) {
                if (files[i].length && !files[i].mapSectors.empty() &&
                    files[i].mapSectors[0] == firstMap && pendingVisibleName(i) == name) {
                    fileIndex = i;
                    break;
                }
            }
            if (fileIndex == files.size()) continue;

            File &file = files[fileIndex];
            if (io->remove(file.hostName.c_str()) < 0) return -EIO;
            for (size_t i = 0; i < file.dataSectors.size(); ++i) {
                uint16_t sector = file.dataSectors[i];
                if (sector < dataRefs.size()) dataRefs[sector] = DataRef();
                if (sector < allocated.size() && allocated[sector]) {
                    allocated[sector] = 0;
                    if (allocatedCount) --allocatedCount;
                }
            }
            for (size_t i = 0; i < file.mapSectors.size(); ++i) {
                uint16_t sector = file.mapSectors[i];
                metadata.erase(sector);
                if (sector < allocated.size() && allocated[sector]) {
                    allocated[sector] = 0;
                    if (allocatedCount) --allocatedCount;
                }
                pendingSectorWrites.erase(sector);
                tracePendingData.erase(sector);
            }
            file.length = 0;
            file.dataSectors.clear();
            file.mapSectors.clear();
            deletedAny = true;
            LOG("SDFS write POC: deleted '%s' after dirent tombstone\n", file.hostName.c_str());
        }
        if (!deletedAny) return 0;

        vector<uint16_t> committedSectors;
        for (map<uint16_t, PendingSectorWrite>::iterator overlay = pendingSectorWrites.begin();
             overlay != pendingSectorWrites.end(); ++overlay) {
            uint16_t sector = overlay->first;
            if (sector >= bitmapStart && sector < bitmapStart + bitmapSectors) {
                size_t offset = (sector - bitmapStart) * sdfsSectorSize;
                memcpy(bitmap.data() + offset, overlay->second.bytes.data(), sdfsSectorSize);
                committedSectors.push_back(sector);
            } else if (metadata.count(sector)) {
                metadata[sector] = overlay->second.bytes;
                committedSectors.push_back(sector);
            }
        }
        for (size_t i = 0; i < committedSectors.size(); ++i)
            pendingSectorWrites.erase(committedSectors[i]);
        return 1;
    }

public:
    explicit DiskStitchImageSDFS(StorageInterface *storage, int sectorCount = maxSectorCount,
                                 const char *label = "DSTITCH", bool enableTraceOverlay = false,
                                 bool enableWritePoc = false)
        : io(storage), sectors(sectorCount), bitmapSectors(0), bitmapStart(0), rootMap(0),
          allocationSearchStart(0), allocatedCount(0), initialized(false), imageIsValid(false),
          volumeName(label ? label : "DSTITCH"), pendingSectorWriteSequence(0),
          traceOverlayEnabled(enableTraceOverlay),
          writePocEnabled(enableWritePoc),
          traceInterleaveDetected(false), traceDataOrderDetected(false), traceWriteCount(0) {
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
        map<uint16_t, PendingSectorWrite>::const_iterator overlay =
            pendingSectorWrites.find(static_cast<uint16_t>(sector));
        if (overlay != pendingSectorWrites.end()) {
            memcpy(buf, overlay->second.bytes.data(), std::min<size_t>(size, overlay->second.bytes.size()));
            return size;
        }
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
                size_t length = std::min<size_t>(static_cast<size_t>(sdfsSectorSize), file.length - offset);
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

    int write(const uint8_t *buf, size_t sector) override {
        if (!initialize() || !buf || sector == 0 || sector > static_cast<size_t>(sectors))
            return -EIO;
        if (traceOverlayEnabled || writePocEnabled) {
            size_t length = specificSectorSize(static_cast<int>(sector));
            vector<uint8_t> before(length, 0);
            size_t readResult = read(before.data(), sector);
            if (static_cast<int>(readResult) < 0) return static_cast<int>(readResult);
            vector<uint8_t> after(buf, buf + length);
            const char *owner = "-";
            string ownerName;
            map<uint16_t, PendingCreate>::iterator pendingForSector = pendingCreates.end();
            for (map<uint16_t, PendingCreate>::iterator i = pendingCreates.begin();
                 i != pendingCreates.end(); ++i)
                if (i->second.mapSector == sector) pendingForSector = i;
            map<uint16_t, PendingAppend>::iterator appendForSector = pendingAppends.end();
            for (map<uint16_t, PendingAppend>::iterator i = pendingAppends.begin();
                 i != pendingAppends.end(); ++i)
                if (i->second.mapSector == sector) appendForSector = i;
            if (sector < dataRefs.size() && dataRefs[sector].mapped) {
                owner = files[dataRefs[sector].fileIndex].hostName.c_str();
            } else if (pendingForSector != pendingCreates.end()) {
                ownerName = pendingForSector->second.name;
                ownerName += "(map)";
                owner = ownerName.c_str();
            } else if (appendForSector != pendingAppends.end()) {
                ownerName = appendForSector->second.name;
                ownerName += "(map)";
                owner = ownerName.c_str();
            }
            //traceSectorWrite(before.data(), after.data(), length, sector, owner);
            stageSectorWrite(static_cast<uint16_t>(sector), after);
            if (writePocEnabled && sector < dataRefs.size() && dataRefs[sector].mapped) {
                const DataRef &ref = dataRefs[sector];
                File &file = files[ref.fileIndex];
                bool replacementPending = false;
                for (map<uint16_t, PendingReplacement>::iterator replacement = pendingReplacements.begin();
                     replacement != pendingReplacements.end(); ++replacement) {
                    if (replacement->second.fileIndex != ref.fileIndex) continue;
                    replacementPending = true;
                }
                if (!replacementPending) {
                    size_t fileOffset = static_cast<size_t>(ref.fileSector) * sdfsSectorSize;
                    if (fileOffset >= file.length) return -EROFS;
                    size_t validBytes = std::min(length, static_cast<size_t>(file.length) - fileOffset);
                    if (io->open(file.hostName.c_str(), O_RDWR) < 0) return -EROFS;
                    int written = io->pwrite(after.data(), validBytes, fileOffset, false);
                    io->close();
                    if (written < 0 || static_cast<size_t>(written) != validBytes) return -EIO;
                }
            }
            vector<TraceCreateIntent> intentsAfter = traceCreateIntents();
            checkTraceInterleaving(intentsAfter);
            if (writePocEnabled) {
                bool rootDirectoryWrite =
                    std::find(rootDirectorySectors.begin(), rootDirectorySectors.end(),
                              static_cast<uint16_t>(sector)) != rootDirectorySectors.end();
                if (rootDirectoryWrite) {
                    int renamed = commitRenamedDirectoryEntries(sector, before.data(), after.data());
                    if (renamed < 0) return renamed;
                    beginPendingReplacement(sector, before.data(), after.data());
                }
                if (rootDirectoryWrite && !stageHypothesisFromDirectoryUpdate(
                        sector, before.data(), after.data())) {
                    fprintf(stderr, "SDFS write POC: unable to resolve changed dirent safely\n");
                    return -EROFS;
                }
                if (rootDirectoryWrite) {
                    int deleted = commitDeletedDirectoryEntries(sector, before.data(), after.data());
                    if (deleted < 0) return deleted;
                }
                if (rootDirectoryWrite) {
                    for (map<uint16_t, PendingAppend>::iterator append = pendingAppends.begin();
                         append != pendingAppends.end();) {
                        if (!append->second.dataSectors.empty()) {
                            ++append;
                            continue;
                        }
                        uint8_t currentMap[sdfsSectorSize];
                        if (static_cast<int>(read(currentMap, append->first)) < 0) return -EROFS;
                        int committed = commitPendingAppend(append->second);
                        if (committed < 0) return committed;
                        if (committed > 0) append = pendingAppends.erase(append);
                        else ++append;
                    }
                }
            }
            if (sector < dataRefs.size() &&
                std::find(rootDirectorySectors.begin(), rootDirectorySectors.end(),
                          static_cast<uint16_t>(sector)) != rootDirectorySectors.end())
                checkPendingDirectory();
            if (traceOverlayEnabled && !writePocEnabled &&
                (tracePendingData.count(static_cast<uint16_t>(sector)) ||
                 pendingCreates.count(static_cast<uint16_t>(sector))))
                checkTraceDataOrder(static_cast<uint16_t>(sector), after.data());
            for (map<uint16_t, PendingCreate>::iterator pending = pendingCreates.begin();
                 writePocEnabled && pending != pendingCreates.end();) {
                uint8_t currentMap[sdfsSectorSize];
                if (static_cast<int>(read(currentMap, pending->first)) < 0) return -EROFS;
                int committed = commitPendingCreate(pending->second);
                if (committed < 0) return committed;
                if (committed > 0) {
                    tracePendingData.erase(pending->first);
                    pending = pendingCreates.erase(pending);
                } else ++pending;
            }
            for (map<uint16_t, PendingAppend>::iterator append = pendingAppends.begin();
                 writePocEnabled && append != pendingAppends.end();) {
                uint8_t currentMap[sdfsSectorSize];
                if (static_cast<int>(read(currentMap, append->first)) < 0) return -EROFS;
                int committed = commitPendingAppend(append->second);
                if (committed < 0) return committed;
                if (committed > 0) append = pendingAppends.erase(append);
                else ++append;
            }
            for (map<uint16_t, PendingReplacement>::iterator replacement = pendingReplacements.begin();
                 writePocEnabled && replacement != pendingReplacements.end();) {
                int committed = commitPendingReplacement(replacement->second);
                if (committed < 0) return committed;
                if (committed > 0) replacement = pendingReplacements.erase(replacement);
                else ++replacement;
            }
            return 0;
        }
        if (sector >= bitmapStart && sector < bitmapStart + bitmapSectors) {
            size_t offset = (sector - bitmapStart) * sdfsSectorSize;
            return memcmp(bitmap.data() + offset, buf, sdfsSectorSize) == 0 ? 0 : -EROFS;
        }
        map<uint16_t, vector<uint8_t> >::const_iterator metadataSector = metadata.find(sector);
        if (metadataSector != metadata.end()) {
            size_t length = std::min<size_t>(specificSectorSize(static_cast<int>(sector)),
                                             metadataSector->second.size());
            return memcmp(metadataSector->second.data(), buf, length) == 0 ? 0 : -EROFS;
        }

        const DataRef &ref = dataRefs[sector];
        if (!ref.mapped) return -EROFS;
        File &file = files[ref.fileIndex];
        size_t offset = static_cast<size_t>(ref.fileSector) * sdfsSectorSize;
        if (offset >= file.length) return -EROFS;
        size_t length = std::min<size_t>(static_cast<size_t>(sdfsSectorSize), file.length - offset);
        if (io->open(file.hostName.c_str(), O_RDWR) < 0) {
            LOG("SDFS: write open denied for '%s'\n", file.hostName.c_str());
            return -EROFS;
        }
        int count = io->pwrite(buf, length, offset, false);
        io->close();
        if (count < 0) return count == -EROFS ? -EROFS : -EIO;
        if (static_cast<size_t>(count) != length) return -EIO;
        LOG("SDFS: wrote %zu bytes to '%s' at %zu\n", length, file.hostName.c_str(), offset);
        return 0;
    }
    int sectorSize() override { return sdfsSectorSize; }
    int sectorCount() override { return sectors; }
    bool valid() override { return initialize(); }
    bool traceInterleaveDetectedForTest() const { return traceInterleaveDetected; }
    bool traceHypothesisFailedForTest() const { return traceDataOrderDetected; }
};

