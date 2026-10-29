// Handles SIO traffic for IDE 2.0 Plus APT hard-drive images.
#ifdef CSIM
#include "csim.h"
#endif
#include "asmDefs.h"

#include <esp_attr.h>

#include "apt.h"

#include <algorithm>
#include <cstring>
#include <esp_attr.h>

#include "diskImage.h"
#include "mmu.h"

using std::min;

namespace {

constexpr uint32_t PBI_IDE_SECTOR_SIZE = 512;
constexpr uint8_t PBI_IDE_MOUNTED = 0x02; // MCB partition-mount flag
constexpr uint8_t PBI_IDE_FEATURE_DYNAMIC_MOUNT = 0x01; // DEVINFO dynamic-mount feature
constexpr uint8_t PBI_IDE_FEATURE_DRIVE_MAP = 0x02; // DEVINFO drive-map feature
constexpr size_t PBI_IDE_MAX_TRANSFER = 1024;
constexpr size_t PBI_IDE_MAX_DRIVES = 15;

struct PbiIdeMountedDrive {
    bool mounted;
    uint16_t partitionId;
    uint32_t startSector;
    uint32_t sectorCount;
};

DRAM_ATTR PbiIdeMountedDrive pbiIdeMountedDrives[PBI_IDE_MAX_DRIVES] = {};
DRAM_ATTR DiskImage *pbiIdeMappedDisk = nullptr;
DRAM_ATTR bool pbiIdeMapInitialized = false;

uint32_t pbiIdeReadLe32(const uint8_t *data);

DiskImage *pbiIdeBackingDisk() {
    DiskImage *disk = atariDisks[7]; // CSIM hard-drive backing image
    return disk != nullptr && disk->valid() ? disk : nullptr;
}

uint64_t pbiIdeBackingSize(DiskImage *disk) {
    if (disk == nullptr || disk->sectorCount() <= 0)
        return 0;
    const uint64_t size = disk->imageSize();
    return size <= 0x7fffffffU ? size : 0; // Largest supported image offset
}

bool pbiIdeReadBytes(DiskImage *disk, uint64_t offset, uint8_t *buffer, size_t length) {
    const uint64_t imageSize = pbiIdeBackingSize(disk);
    if (disk == nullptr || offset > imageSize || length > imageSize - offset)
        return false;

    size_t copied = 0;
    while (copied < length) {
        const uint64_t current = offset + copied;
        const int sector = disk->offsetToSector(static_cast<int>(current));
        const int sectorOffset = disk->sectorToOffset(sector);
        const int sectorSize = disk->specificSectorSize(sector);
        if (sector < 1 || sectorSize <= 0 || sectorSize > static_cast<int>(PBI_IDE_SECTOR_SIZE) ||
            current < static_cast<uint64_t>(sectorOffset) ||
            current >= static_cast<uint64_t>(sectorOffset) + sectorSize)
            return false;

        uint8_t sectorBuffer[PBI_IDE_SECTOR_SIZE];
        if (disk->read(sectorBuffer, sector) < static_cast<size_t>(sectorSize))
            return false;
        const size_t inSector = static_cast<size_t>(current - sectorOffset);
        const size_t chunk = min(length - copied, static_cast<size_t>(sectorSize) - inSector);
        memcpy(buffer + copied, sectorBuffer + inSector, chunk);
        copied += chunk;
    }
    return true;
}

bool pbiIdeWriteBytes(DiskImage *disk, uint64_t offset, const uint8_t *buffer, size_t length) {
    const uint64_t imageSize = pbiIdeBackingSize(disk);
    if (disk == nullptr || offset > imageSize || length > imageSize - offset)
        return false;

    size_t copied = 0;
    while (copied < length) {
        const uint64_t current = offset + copied;
        const int sector = disk->offsetToSector(static_cast<int>(current));
        const int sectorOffset = disk->sectorToOffset(sector);
        const int sectorSize = disk->specificSectorSize(sector);
        if (sector < 1 || sectorSize <= 0 || sectorSize > static_cast<int>(PBI_IDE_SECTOR_SIZE) ||
            current < static_cast<uint64_t>(sectorOffset) ||
            current >= static_cast<uint64_t>(sectorOffset) + sectorSize)
            return false;

        uint8_t sectorBuffer[PBI_IDE_SECTOR_SIZE];
        if (disk->read(sectorBuffer, sector) < static_cast<size_t>(sectorSize))
            return false;
        const size_t inSector = static_cast<size_t>(current - sectorOffset);
        const size_t chunk = min(length - copied, static_cast<size_t>(sectorSize) - inSector);
        memcpy(sectorBuffer + inSector, buffer + copied, chunk);
        if (disk->write(sectorBuffer, sector) < sectorSize)
            return false;
        copied += chunk;
    }
    return true;
}

void pbiIdeLoadAptDriveMap(DiskImage *disk) {
    if (pbiIdeMapInitialized && pbiIdeMappedDisk == disk)
        return;

    memset(pbiIdeMountedDrives, 0, sizeof(pbiIdeMountedDrives));
    pbiIdeMappedDisk = disk;
    pbiIdeMapInitialized = true;

    const uint64_t diskSectors = pbiIdeBackingSize(disk) / PBI_IDE_SECTOR_SIZE;
    if (diskSectors == 0)
        return;

    uint8_t table[PBI_IDE_SECTOR_SIZE];
    if (!pbiIdeReadBytes(disk, 0, table, sizeof(table)))
        return;

    bool aptFound = table[1] == 0x41 && table[2] == 0x50 && table[3] == 0x54; // APT signature
    if (!aptFound) {
        if (table[510] != 0x55 || table[511] != 0xaa) // MBR signature
            return;
        for (size_t index = 0; index < 4; ++index) {
            const size_t entry = 0x1be + index * 16; // MBR partition entry
            if (table[entry + 4] != 0x7f) // Protective APT container
                continue;
            const uint32_t start = pbiIdeReadLe32(table + entry + 8);
            if (start >= diskSectors ||
                !pbiIdeReadBytes(disk, static_cast<uint64_t>(start) * PBI_IDE_SECTOR_SIZE,
                                 table, sizeof(table)))
                return;
            if (table[1] == 0x41 && table[2] == 0x50 && table[3] == 0x54) { // APT signature
                aptFound = true;
                break;
            }
        }
        if (!aptFound)
            return;
    }

    const uint8_t entriesInSector = table[5];
    if (entriesInSector < PBI_IDE_MAX_DRIVES + 1 || entriesInSector > 32)
        return;

    for (size_t driveIndex = 0; driveIndex < PBI_IDE_MAX_DRIVES; ++driveIndex) {
        const size_t entry = 16 + driveIndex * 16; // Mapping slot; index is D1-D15
        const uint8_t *mapping = table + entry;
        const uint8_t partitionType = mapping[1]; // APT partition type
        const uint32_t startSector = pbiIdeReadLe32(mapping + 2); // Absolute start LBA
        const uint32_t sectorCount = pbiIdeReadLe32(mapping + 6); // Physical sector count
        const uint16_t partitionId = static_cast<uint16_t>(mapping[10] | // Mapping source ID low
            (static_cast<uint16_t>(mapping[11]) << 8)); // Mapping source ID high
        const bool dosPartition = partitionType == 0x00 || partitionType == 0x03;
        const bool active = (mapping[12] & 0x40) != 0; // DOS automount flag
        if (mapping[0] == 0 || !dosPartition || !active || partitionId == 0 ||
            partitionId == 0xffff || startSector == 0 || sectorCount == 0 ||
            startSector >= diskSectors || sectorCount > diskSectors - startSector)
            continue;

        PbiIdeMountedDrive &mounted = pbiIdeMountedDrives[driveIndex];
        mounted.mounted = true;
        mounted.partitionId = partitionId;
        mounted.startSector = startSector;
        mounted.sectorCount = sectorCount;
    }
}

uint32_t pbiIdeReadLe32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

void pbiIdeWriteLe32(uint8_t *data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}

uint16_t pbiIdeDriveMap() {
    uint16_t driveMap = 0;
    for (size_t index = 0; index < PBI_IDE_MAX_DRIVES; ++index) {
        if (pbiIdeMountedDrives[index].mounted)
            driveMap |= static_cast<uint16_t>(1U << index);
    }
    return driveMap;
}

void pbiIdeWriteAtaWord(uint8_t *data, size_t word, uint16_t value) {
    data[word * 2] = static_cast<uint8_t>(value);
    data[word * 2 + 1] = static_cast<uint8_t>(value >> 8);
}

void pbiIdeWriteAtaString(uint8_t *data, size_t offset, size_t length, const char *value) {
    const size_t valueLength = strlen(value);
    for (size_t i = 0; i < length; i += 2) {
        const uint8_t first = i < valueLength ? static_cast<uint8_t>(value[i]) : ' ';
        const uint8_t second = i + 1 < valueLength ? static_cast<uint8_t>(value[i + 1]) : ' ';
        data[offset + i] = second;
        data[offset + i + 1] = first;
    }
}

bool pbiIdeMountPartition(const uint8_t *mcb, DiskImage *disk) {
    unsigned int drive = mcb[1]; // MCB drive number
        if (mcb[0] != PBI_IDE_MOUNTED) // MCB flags: partition mount
        return false;
    if (drive >= '1' && drive <= '9')
        drive -= '0';
    const uint32_t partitionId = pbiIdeReadLe32(mcb + 2); // MCB partition Sub-ID
    if (drive < 1 || drive > PBI_IDE_MAX_DRIVES || partitionId < 1 || partitionId > 4)
        return false;

    uint8_t mbr[PBI_IDE_SECTOR_SIZE];
    if (!pbiIdeReadBytes(disk, 0, mbr, sizeof(mbr)) || mbr[510] != 0x55 || mbr[511] != 0xaa) // MBR signature
        return false;
    const size_t entry = 0x1be + (partitionId - 1) * 16; // MBR partition entry
    const uint32_t startSector = pbiIdeReadLe32(mbr + entry + 8); // First LBA
    const uint32_t sectorCount = pbiIdeReadLe32(mbr + entry + 12); // LBA count
    const uint64_t diskSectors = pbiIdeBackingSize(disk) / PBI_IDE_SECTOR_SIZE;
    if (mbr[entry + 4] == 0 || sectorCount == 0 || startSector >= diskSectors || // Empty MBR type
        sectorCount > diskSectors - startSector)
        return false;

    PbiIdeMountedDrive &mounted = pbiIdeMountedDrives[drive - 1];
    mounted.mounted = true;
    mounted.partitionId = static_cast<uint16_t>(partitionId);
    mounted.startSector = startSector;
    mounted.sectorCount = sectorCount;
    return true;
}

void pbiIdeFillDiskInfo(uint8_t *buffer, size_t length, const PbiIdeMountedDrive *mounted,
                        uint32_t diskSectors) {
    memset(buffer, 0, length);
    buffer[0] = 1; // Bus ID
    buffer[1] = 0; // Controller ID
    buffer[2] = 1; // Master device ID
    if (mounted != nullptr) {
        buffer[3] = static_cast<uint8_t>(mounted->partitionId); // Device Sub-ID low
        buffer[4] = static_cast<uint8_t>(mounted->partitionId >> 8); // Device Sub-ID high
        buffer[6] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE); // Sector size low
        buffer[7] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE >> 8); // Sector size high
        buffer[8] = static_cast<uint8_t>(mounted->sectorCount); // Sector count byte 0
        buffer[9] = static_cast<uint8_t>(mounted->sectorCount >> 8); // Sector count byte 1
        buffer[10] = static_cast<uint8_t>(mounted->sectorCount >> 16); // Sector count byte 2
        buffer[11] = static_cast<uint8_t>(mounted->sectorCount >> 24); // Sector count byte 3
        buffer[12] = static_cast<uint8_t>(mounted->startSector); // Start LBA byte 0
        buffer[13] = static_cast<uint8_t>(mounted->startSector >> 8); // Start LBA byte 1
        buffer[14] = static_cast<uint8_t>(mounted->startSector >> 16); // Start LBA byte 2
        buffer[15] = static_cast<uint8_t>(mounted->startSector >> 24); // Start LBA byte 3
    } else {
        buffer[3] = PBI_IDE_FEATURE_DYNAMIC_MOUNT | PBI_IDE_FEATURE_DRIVE_MAP; // Feature table
        buffer[6] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE); // Sector size low
        buffer[7] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE >> 8); // Sector size high
        pbiIdeWriteLe32(buffer + 8, diskSectors);
        memcpy(buffer + 0x10, "GENERIC 16M SSD", sizeof("GENERIC 16M SSD")); // Device name
    }
}

bool pbiIdeSetPercom(uint8_t *buffer, size_t length, uint32_t sectorCount) {
    if (length != 12 || sectorCount > 0xffff)
        return false;
    memset(buffer, 0, 12);
    buffer[0] = 1; // PERCOM device type
    buffer[1] = 3; // PERCOM tracks
    buffer[2] = static_cast<uint8_t>(sectorCount >> 8); // Sectors/track high
    buffer[3] = static_cast<uint8_t>(sectorCount); // Sectors/track low
    buffer[4] = 1; // Disk sides
    buffer[5] = 4; // Density code
    buffer[6] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE >> 8); // Bytes/sector high
    buffer[7] = static_cast<uint8_t>(PBI_IDE_SECTOR_SIZE); // Bytes/sector low
    buffer[8] = 0xff; // PERCOM flags
    return true;
}
}


int handlePbiAptDcb(PbiIocb *request, AtariDCB *dcb) {
    const uint8_t device = dcb->DDEVIC;
    const uint8_t command = dcb->DCOMND;
    const uint8_t unit = dcb->DUNIT;
    const uint16_t address = static_cast<uint16_t>(dcb->DBUFLO | (dcb->DBUFHI << 8)); // DCB buffer address
    const uint16_t length = static_cast<uint16_t>(dcb->DBYTLO | (dcb->DBYTHI << 8)); // DCB transfer length
    const uint32_t sector = static_cast<uint32_t>(dcb->DAUX1) |
        (static_cast<uint32_t>(dcb->DAUX2) << 8) |
            (device == 0x20 ? 0 : static_cast<uint32_t>(atariRam[0x30c]) << 16) | // DAUX3 for $31 requests
        (device == 0x20 ? 0 : static_cast<uint32_t>(atariRam[0x30d]) << 24); // DAUX4 high LBA
    DiskImage *disk = pbiIdeBackingDisk();
    if (disk != nullptr)
        pbiIdeLoadAptDriveMap(disk);
    const bool mountedLogicalDrive = device == 0x31 && unit >= 1 && unit <= PBI_IDE_MAX_DRIVES &&
        pbiIdeMountedDrives[unit - 1].mounted;
    const bool recognizedDevice = device == 0xa0 || device == 0xb1 || device == 0x20 || // Physical IDs
        (device == 0x31 && command == 0x6e) || mountedLogicalDrive; // Logical disk-info or mounted DUNIT
    if (!recognizedDevice)
        return -1;
    if (disk == nullptr)
        return -1;

    if (command == 0x46 && (device == 0x20 || mountedLogicalDrive)) { // Force media change
        dcb->DSTATS = 1;
        request->carry = 1;
        return RES_FLAG_COMPLETE;
    }

    request->copybuf = address;
    request->copylen = length;
    if (static_cast<uint32_t>(address) + length > 0x10000U) {
        dcb->DSTATS = 0;
        request->carry = 0;
        return RES_FLAG_COMPLETE;
    }
    uint8_t *buffer = mmuCheckRangeMapped(mmuStateSaved, address, length);
    const bool copyRequired = buffer == nullptr;
    if (copyRequired && length > PBI_IDE_MAX_TRANSFER) {
        dcb->DSTATS = 0;
        request->carry = 0;
        return RES_FLAG_COMPLETE;
    }
    if (copyRequired)
        buffer = &pbiROM[0x400];

    auto finish = [&](bool success, bool copyout = false) {
        dcb->DSTATS = success ? 1 : 0;
        request->carry = success ? 1 : 0;
        return success && copyRequired && copyout ? RES_FLAG_COPYOUT : RES_FLAG_COMPLETE;
    };
    auto needsCopyIn = [&]() {
        if (copyRequired && (request->req & REQ_FLAG_COPYIN) == 0) {
            request->result |= RES_FLAG_NEED_COPYIN;
            return true;
        }
        return false;
    };

    const uint32_t diskSectors = static_cast<uint32_t>(pbiIdeBackingSize(disk) / PBI_IDE_SECTOR_SIZE);
    PbiIdeMountedDrive *mounted = nullptr;
    bool physical = device == 0xa0 || device == 0x20; // Physical device IDs
    bool logicalDevice = device == 0xb1 || device == 0x31; // Logical drive IDs
    uint8_t drive = unit;
    if (logicalDevice) {
        if (drive >= 1 && drive <= PBI_IDE_MAX_DRIVES && pbiIdeMountedDrives[drive - 1].mounted)
            mounted = &pbiIdeMountedDrives[drive - 1];
        else
            return finish(false);
    }
    if (physical && unit != 1)
        return finish(false);

    switch (command) {
    case 0x4e: // Read PERCOM
        return finish(pbiIdeSetPercom(buffer, length, mounted ? mounted->sectorCount : diskSectors), true);
    case 0x6e: // DEVINFO / DISKINFO
        if (physical) {
            if (length != 512)
                return finish(false);
            pbiIdeFillDiskInfo(buffer, length, nullptr, diskSectors);
        } else {
            if (length != 64)
                return finish(false);
            if (mounted == nullptr)
                return finish(false);
            pbiIdeFillDiskInfo(buffer, length, mounted, diskSectors);
        }
        return finish(true, true);
    case 0x4d: { // Mount / drive-map query
        if (length != 6)
            return finish(false);
        if (needsCopyIn())
            return RES_FLAG_NEED_COPYIN;
        if ((buffer[0] & 0x20) != 0) { // MCB drive-map query flag
            const uint16_t driveMap = pbiIdeDriveMap();
            buffer[0] = static_cast<uint8_t>(driveMap); // Drive map low byte
            buffer[1] = static_cast<uint8_t>(driveMap >> 8); // Drive map high byte
            return finish(true, true);
        }
        return finish(pbiIdeMountPartition(buffer, disk));
    }
    case 0xec: // ATA IDENTIFY DEVICE
        if (!physical || length != 512)
            return finish(false);
        memset(buffer, 0, 512);
        pbiIdeWriteAtaWord(buffer, 1, 64); // Cylinders
        pbiIdeWriteAtaWord(buffer, 3, 16); // Heads
        pbiIdeWriteAtaWord(buffer, 6, 32); // Sectors/track
        pbiIdeWriteAtaString(buffer, 20, 20, "00000000000000000001"); // Serial number
        pbiIdeWriteAtaString(buffer, 54, 40, "GENERIC 16MB SSD"); // Model name
        pbiIdeWriteAtaWord(buffer, 49, 0x0200); // LBA capability
        pbiIdeWriteAtaWord(buffer, 53, 1); // Validity of words 54-58
        pbiIdeWriteAtaWord(buffer, 54, 64); // Current cylinders
        pbiIdeWriteAtaWord(buffer, 55, 16); // Current heads
        pbiIdeWriteAtaWord(buffer, 56, 32); // Current sectors/track
        pbiIdeWriteAtaWord(buffer, 57, static_cast<uint16_t>(diskSectors)); // Current capacity low
        pbiIdeWriteAtaWord(buffer, 58, static_cast<uint16_t>(diskSectors >> 16)); // Current capacity high
        pbiIdeWriteAtaWord(buffer, 59, 0x0120); // Multiple-sector setting
        pbiIdeWriteAtaWord(buffer, 60, static_cast<uint16_t>(diskSectors)); // LBA capacity low
        pbiIdeWriteAtaWord(buffer, 61, static_cast<uint16_t>(diskSectors >> 16)); // LBA capacity high
        return finish(true, true);
    case 0x53: // Device/disk status
        if (length != 4)
            return finish(false);
        if (physical) {
            buffer[0] = 0x40; // Device present
            buffer[1] = 0x00; // No IDE error
        } else {
            buffer[0] = 0x20; // 512-byte density
            buffer[1] = 0xff; // No IDE error
        }
        buffer[2] = 0x01; // IDE Plus handler ID
        buffer[3] = 0x20; // Handler version 2.0
        return finish(true, true);
    case 0x52: // Read sector
    case 0xd2: { // Read sector, extended LBA
        const uint64_t lba = mounted ? static_cast<uint64_t>(mounted->startSector) + sector - 1 : sector;
        const uint64_t count = length / PBI_IDE_SECTOR_SIZE;
        if (length == 0 || length % PBI_IDE_SECTOR_SIZE != 0 ||
            (!physical && sector == 0) || lba >= diskSectors || count > diskSectors - lba ||
            (mounted != nullptr && (sector - 1 >= mounted->sectorCount ||
             count > mounted->sectorCount - (sector - 1))) ||
            !pbiIdeReadBytes(disk, lba * PBI_IDE_SECTOR_SIZE, buffer, length))
            return finish(false);
        return finish(true, true);
    }
    case 0x50: // Write sector
    case 0xd0: // Write sector, extended LBA
    case 0x57: // Write multiple
    case 0xd7: { // Write multiple, extended LBA
        const uint64_t lba = mounted ? static_cast<uint64_t>(mounted->startSector) + sector - 1 : sector;
        const uint64_t count = length / PBI_IDE_SECTOR_SIZE;
        if (length == 0 || length % PBI_IDE_SECTOR_SIZE != 0 ||
            (!physical && sector == 0) || lba >= diskSectors || count > diskSectors - lba ||
            (mounted != nullptr && (sector - 1 >= mounted->sectorCount ||
             count > mounted->sectorCount - (sector - 1))))
            return finish(false);
        if (needsCopyIn())
            return RES_FLAG_NEED_COPYIN;
        return finish(pbiIdeWriteBytes(disk, lba * PBI_IDE_SECTOR_SIZE, buffer, length));
    }
    default:
        return finish(false);
    }
}
