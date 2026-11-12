#ifdef CSIM
#include "csim.h"
#endif

#include "diskFlash.h"
#include "esp_heap_caps.h"
#include "gzipImage.h"

#include <algorithm>
#include <string>
#include <vector>
#include <strings.h>
using std::vector;
using std::string;

// returns TRUE if text string matches wild pattern with * and ?
bool naive_recursive_match(const char *text, const char *wild) {
  while (*text != '\0') {
    if (*wild == '*') {
      // any number of stars act as one star
      while (*++wild == '*')
        continue;
      // a star at the end of the pattern matches any text
      if (*wild == '\0')
        return true;
      // star-loop: match the rest of the pattern and text
      while (naive_recursive_match(text, wild) == false && *text != '\0')
        text++;
      return *text != '\0';
    }
    // ? matches any character or we match the current non-NUL character
    if (*wild != '?' && toupper(*wild) != toupper(*text))
      return false;
    text++;
    wild++;
  }
  // ignore trailing stars
  while (*wild == '*')
    wild++;
  // at end of text means success if nothing else is left to match
  return *wild == '\0';
}

vector<string> listFiles(const char *directory, const char *pattern) {
    vector<string> result;
    DIR *dir = opendir(directory);
    if (dir == nullptr) return result;
    while (struct dirent *entry = readdir(dir)) {
        if (naive_recursive_match(entry->d_name, pattern))
            result.push_back(string("/") + entry->d_name);
    }
    closedir(dir);
    return result;
}

namespace {

// Read exactly len bytes (the VFS may return less); false on a short read.
bool readFully(int fd, void *buf, size_t len) {
    uint8_t *p = static_cast<uint8_t *>(buf);
    while (len > 0) {
        ssize_t got = read(fd, p, len);
        if (got <= 0) return false;
        p += got;
        len -= static_cast<size_t>(got);
    }
    return true;
}

}  // namespace

void DiskImageATR::open(const char *f, bool cacheInPsram) {
    image = NULL;
    imageAllocation = NULL;
    header = AtrImageHeader{};   // valid() only once a good header is loaded
    struct stat info;
    if (stat(f, &info) != 0 || (fd = ::open(f, O_RDWR)) < 0) {
        printf("AtariDisk::open('%s'): file open failed\n", f);
        return;
    }
    size_t fsize = static_cast<size_t>(info.st_size);
    size_t pathLength = strlen(f);
    bool compressed = pathLength >= 7 && strcasecmp(f + pathLength - 7, ".atr.gz") == 0;
    if (compressed && !cacheInPsram) {
        printf("DiskImage::open('%s'): compressed ATR requires PSRAM caching\n", f);
        ::close(fd);
        fd = -1;
        return;
    }

    if (compressed) {
        const GzipReadFn readFile = [this](uint8_t *buf, size_t len) {
            return static_cast<int>(::read(fd, buf, len));
        };
        GzipImageInfo gzipInfo;
        size_t maxOutputSize = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (!gzipImageInflate(readFile, fsize, nullptr, 0, maxOutputSize, gzipInfo) ||
            gzipInfo.outputSize < sizeof(header)) {
            printf("DiskImage::open('%s'): invalid or oversized gzip ATR\n", f);
            ::close(fd);
            fd = -1;
            return;
        }
        memcpy(&header, gzipInfo.prefix, sizeof(header));
        uint64_t paragraphs = static_cast<uint64_t>(header.pars) +
                              static_cast<uint64_t>(header.parsHigh) * 0x10000;
        uint64_t dataSize64 = paragraphs * 0x10;
        if (header.magic != 0x0296 || header.sectorSize == 0 ||
            dataSize64 > SIZE_MAX || gzipInfo.outputSize != sizeof(header) + dataSize64) {
            printf("DiskImage::open('%s'): invalid compressed ATR header or size\n", f);
            ::close(fd);
            header = AtrImageHeader{};   // not valid(): no stale geometry
            fd = -1;
            return;
        }

        size_t expandedSize = gzipInfo.outputSize;
        imageAllocation = static_cast<uint8_t *>(
            heap_caps_malloc(expandedSize + 1, MALLOC_CAP_SPIRAM));
        if (!imageAllocation) {
            printf("DiskImage::open('%s'): PSRAM allocation of %u bytes failed\n", f,
                   static_cast<unsigned>(expandedSize));
            heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
            ::close(fd);
            header = AtrImageHeader{};   // not valid(): no stale geometry
            fd = -1;
            return;
        }
        GzipImageInfo loadedInfo;
        bool loaded = lseek(fd, 0, SEEK_SET) >= 0 &&
                      gzipImageInflate(readFile, fsize, imageAllocation, expandedSize + 1,
                                       expandedSize, loadedInfo) &&
                      loadedInfo.outputSize == gzipInfo.outputSize &&
                      loadedInfo.outputCrc == gzipInfo.outputCrc &&
                      memcmp(loadedInfo.prefix, gzipInfo.prefix, sizeof(gzipInfo.prefix)) == 0;
        ::close(fd);
        fd = -1;
        if (!loaded) {
            printf("DiskImage::open('%s'): compressed ATR read/decompression failed\n", f);
            heap_caps_free(imageAllocation);
            header = AtrImageHeader{};   // not valid(): no stale geometry
            imageAllocation = NULL;
            return;
        }
        image = imageAllocation + sizeof(header);
        printf("DiskImage::open('%s'): loaded %u-byte gzip ATR into PSRAM\n", f,
               static_cast<unsigned>(expandedSize));
        filename = f;
        return;
    }

    if (!readFully(fd, &header, sizeof(header)) || header.magic != 0x0296) {
        printf("DiskImage::open('%s'): bad file or bad atr header\n", f);
        ::close(fd);
        header = AtrImageHeader{};   // not valid(): no stale geometry
        fd = -1;
        return;
    }
    printf("DiskImage::open('%s'): sector size %d, header size %zu, file size %zu,"
           " image size %zu\n", f, header.sectorSize, sizeof(header), fsize,
           (size_t)(header.pars + header.parsHigh * 0x10000) * 0x10);
    if (cacheInPsram) { 
        size_t dataSize = (header.pars + header.parsHigh * 0x10000) * 0x10;
        image = (uint8_t *)heap_caps_malloc(dataSize, MALLOC_CAP_SPIRAM);
        if (image == NULL) {
            printf("DiskImage::open('%s'): psram heap_caps_malloc(%zu) failed!\n", f, dataSize);
            heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
            ::close(fd);
            header = AtrImageHeader{};   // not valid(): no stale geometry
            fd = -1;
            return;
        }
        imageAllocation = image;
        printf("Opened '%s' file size %zu bytes, reading data: ", f, fsize);
        bool ok = lseek(fd, sizeof(header), SEEK_SET) >= 0 && readFully(fd, image, dataSize);
        ::close(fd);
        fd = -1;
        if (!ok) {
            printf("wrong size, discarding\n");
            heap_caps_free(image);
            header = AtrImageHeader{};   // not valid(): no stale geometry
            image = NULL;
            imageAllocation = NULL;
            return;
        }
        printf("OK\n");
    } 
    filename = f;
}
bool IRAM_ATTR DiskImageATR::valid() { return header.magic == 0x0296; }

int IRAM_ATTR DiskImageATR::sectorSize() { return header.sectorSize; }

int IRAM_ATTR DiskImageATR::sectorCount() { return (header.pars + header.parsHigh * 256) * 0x10 / header.sectorSize; }

void DiskImageATR::close() {
    if (imageAllocation != NULL) {
        heap_caps_free(imageAllocation);
        imageAllocation = NULL;
        image = NULL;
    } else if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

size_t IRAM_ATTR DiskImageATR::read(uint8_t *buf, size_t sector) {
    if (sector == 0 || header.sectorSize == 0) return 0;
    int secSize = header.sectorSize;
    // first 3 sectors are always 128 bytes even on DD disks
    if (sector <= 3) secSize = 128;
    int offset = (sector - 1) * secSize;
    if (sector > 3) offset -= 3 * (secSize - 128);
    if(image != NULL) {
        for(int n = 0; n < secSize; n++) 
            buf[n] = image[offset + n];
        return secSize;
    } else if(filename.length() != 0) {
        if (lseek(fd, offset + sizeof(header), SEEK_SET) < 0)
            return 0;
        return readFully(fd, buf, secSize) ? secSize : 0;
    }
    return 0;
}

int IRAM_ATTR DiskImageATR::write(const uint8_t *buf, size_t sector) {
    if (sector == 0 || header.sectorSize == 0) return -1;
    int secSize = header.sectorSize;
    // first 3 sectors are always 128 bytes even on DD disks
    if (sector <= 3) secSize = 128;
    int offset = (sector - 1) * secSize;
    if (sector > 3) offset -= 3 * (secSize - 128);
    if(image != NULL) {
        for(int n = 0; n < secSize; n++) image[offset + n] = buf[n];
        return secSize;
    } else if(filename.length() != 0) {
        if (lseek(fd, offset + sizeof(header), SEEK_SET) < 0)
            return -1;
        return static_cast<int>(::write(fd, buf, secSize));
    }
    return 0;
}

#ifdef CSIM
DiskImageIMG::~DiskImageIMG() { heap_caps_free(image); }

void DiskImageIMG::open(const char *f) {
    image = nullptr;
    imageBytes = 0;

    struct stat info;
    if (stat(f, &info) != 0 || info.st_size == 0 ||
        info.st_size % 256 != 0 || info.st_size > INT_MAX) {
        printf("DiskImageIMG::open('%s'): invalid raw image size\n", f);
        return;
    }
    const size_t size = static_cast<size_t>(info.st_size);

    int fd = ::open(f, O_RDONLY);
    if (fd < 0) {
        printf("DiskImageIMG::open('%s'): file open failed\n", f);
        return;
    }

    image = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (image == nullptr) {
        printf("DiskImageIMG::open('%s'): PSRAM allocation of %u bytes failed\n", f,
               static_cast<unsigned>(size));
        ::close(fd);
        return;
    }

    bool ok = readFully(fd, image, size);
    ::close(fd);
    if (!ok) {
        printf("DiskImageIMG::open('%s'): short image read\n", f);
        heap_caps_free(image);
        image = nullptr;
        return;
    }
    imageBytes = size;
    filename = f;
    printf("DiskImageIMG::open('%s'): read %zu bytes\n", f, imageBytes);
}

bool DiskImageIMG::valid() { return image != nullptr && imageBytes != 0; }
int DiskImageIMG::sectorSize() { return 256; }
int DiskImageIMG::sectorCount() { return static_cast<int>(imageBytes / 256); }
int DiskImageIMG::specificSectorSize(int) { return 256; }
int DiskImageIMG::offsetToSector(int offset) { return offset / 256 + 1; }
int DiskImageIMG::sectorToOffset(int sector) { return (sector - 1) * 256; }
size_t DiskImageIMG::imageSize() { return imageBytes; }

size_t DiskImageIMG::read(uint8_t *buf, size_t sector) {
    if (!valid() || sector == 0 || sector > imageBytes / 256) return 0;
    memcpy(buf, image + (sector - 1) * 256, 256);
    return 256;
}

int DiskImageIMG::write(const uint8_t *buf, size_t sector) {
    if (!valid() || sector == 0 || sector > imageBytes / 256) return -1;
    memcpy(image + (sector - 1) * 256, buf, 256);
    return 256;
}
#endif
