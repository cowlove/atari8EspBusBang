#include "gzipImage.h"

#include "esp_heap_caps.h"
#include "miniz.h"

#include <algorithm>
#include <string.h>

namespace {

constexpr size_t GzipInputSize = 1024;
constexpr size_t GzipWindowSize = TINFL_LZ_DICT_SIZE;

struct GzipReader {
    spiffs *fs;
    spiffs_file fd;
    size_t fileSize;
    size_t readLimit;
    size_t fetched = 0;
    size_t position = 0;
    size_t bufferOffset = 0;
    size_t bufferSize = 0;
    uint8_t *buffer;

    GzipReader(spiffs *filesystem, spiffs_file file, size_t size)
        : fs(filesystem), fd(file), fileSize(size), readLimit(size),
          buffer(static_cast<uint8_t *>(heap_caps_malloc(GzipInputSize, MALLOC_CAP_SPIRAM))) {}

    ~GzipReader() {
        if (buffer) heap_caps_free(buffer);
    }

    bool fill() {
        if (bufferOffset < bufferSize) return true;
        if (fetched >= readLimit) return false;
        size_t request = std::min(GzipInputSize, readLimit - fetched);
        int result = SPIFFS_read(fs, fd, buffer, request);
        if (result <= 0) return false;
        fetched += result;
        bufferOffset = 0;
        bufferSize = result;
        return true;
    }

    size_t available() const {
        size_t remaining = position < readLimit ? readLimit - position : 0;
        return std::min(bufferSize - bufferOffset, remaining);
    }

    void consume(size_t count) {
        bufferOffset += count;
        position += count;
    }

    bool read(void *destination, size_t count, uint32_t *crc = nullptr) {
        auto *output = static_cast<uint8_t *>(destination);
        while (count > 0) {
            if (!fill()) return false;
            size_t amount = std::min(count, available());
            if (amount == 0) return false;
            if (output) {
                memcpy(output, buffer + bufferOffset, amount);
                output += amount;
            }
            if (crc) *crc = static_cast<uint32_t>(mz_crc32(*crc, buffer + bufferOffset, amount));
            consume(amount);
            count -= amount;
        }
        return true;
    }

    bool atEnd() const {
        return position == fileSize && fetched == fileSize && bufferOffset == bufferSize;
    }
};

bool readGzipHeader(GzipReader &reader) {
    uint8_t fixedHeader[10];
    uint32_t headerCrc = MZ_CRC32_INIT;
    if (!reader.read(fixedHeader, sizeof(fixedHeader), &headerCrc)) return false;
    if (fixedHeader[0] != 0x1f || fixedHeader[1] != 0x8b || fixedHeader[2] != 8 ||
        (fixedHeader[3] & 0xe0) != 0) return false;

    uint8_t flags = fixedHeader[3];
    uint8_t scratch[64];
    if (flags & 0x04) {
        uint8_t extraLengthBytes[2];
        if (!reader.read(extraLengthBytes, sizeof(extraLengthBytes), &headerCrc)) return false;
        size_t extraLength = extraLengthBytes[0] | (static_cast<size_t>(extraLengthBytes[1]) << 8);
        while (extraLength > 0) {
            size_t amount = std::min(extraLength, sizeof(scratch));
            if (!reader.read(scratch, amount, &headerCrc)) return false;
            extraLength -= amount;
        }
    }
    for (uint8_t optionalFlag : {uint8_t(0x08), uint8_t(0x10)}) {
        if (flags & optionalFlag) {
            uint8_t byte;
            do {
                if (!reader.read(&byte, 1, &headerCrc)) return false;
            } while (byte != 0);
        }
    }
    if (flags & 0x02) {
        uint8_t expectedCrc[2];
        if (!reader.read(expectedCrc, sizeof(expectedCrc))) return false;
        uint16_t value = expectedCrc[0] | (static_cast<uint16_t>(expectedCrc[1]) << 8);
        if (value != (headerCrc & 0xffff)) return false;
    }
    return true;
}

}

bool gzipImageInflate(spiffs *fs, spiffs_file fd, size_t fileSize, uint8_t *output,
                      size_t outputCapacity, size_t maxOutputSize, GzipImageInfo &info) {
    GzipReader reader{fs, fd, fileSize};
    if (!reader.buffer || !readGzipHeader(reader) || fileSize < reader.position + 8) return false;
    reader.readLimit = fileSize - 8;

    auto *decompressor = static_cast<tinfl_decompressor *>(
        heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM));
    auto *window = output ? nullptr : static_cast<uint8_t *>(
        heap_caps_malloc(GzipWindowSize, MALLOC_CAP_SPIRAM));
    if (!decompressor || (!output && !window)) {
        if (decompressor) heap_caps_free(decompressor);
        if (window) heap_caps_free(window);
        return false;
    }
    tinfl_init(decompressor);

    size_t windowOffset = 0;
    tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;
    uint32_t outputCrc = MZ_CRC32_INIT;
    size_t outputSize = 0;
    bool valid = false;

    while (true) {
        if (reader.available() == 0 && !reader.fill()) break;
        if (!output && windowOffset == GzipWindowSize) windowOffset = 0;
        size_t inSize = reader.available();
        size_t outSize = output ? outputCapacity - outputSize : GzipWindowSize - windowOffset;
        if (outSize == 0) break;
        uint8_t *outStart = output ? output : window;
        uint8_t *outNext = outStart + (output ? outputSize : windowOffset);
        mz_uint32 flags = TINFL_FLAG_HAS_MORE_INPUT;
        if (output) flags |= TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;

        size_t inputBefore = inSize;
        size_t outputBefore = outSize;
        status = tinfl_decompress(decompressor, reader.buffer + reader.bufferOffset, &inSize,
                                  outStart, outNext, &outSize, flags);
        reader.consume(inSize);
        size_t produced = outSize;
        uint8_t *producedAt = outNext;
        if (produced > 0) {
            if (produced > maxOutputSize - std::min(outputSize, maxOutputSize)) break;
            outputCrc = static_cast<uint32_t>(mz_crc32(outputCrc, producedAt, produced));
            size_t prefixCount = std::min(produced, sizeof(info.prefix) -
                                                      std::min(outputSize, sizeof(info.prefix)));
            if (prefixCount) memcpy(info.prefix + outputSize, producedAt, prefixCount);
            outputSize += produced;
            if (!output) windowOffset += produced;
        }
        if (status == TINFL_STATUS_DONE) {
            valid = true;
            break;
        }
        if (status < TINFL_STATUS_DONE) break;
        if (inputBefore == inSize && outputBefore == outSize && reader.available() > 0) break;
    }

    uint8_t trailer[8];
    if (valid && reader.position != reader.readLimit) valid = false;
    reader.readLimit = reader.fileSize;
    if (valid && reader.read(trailer, sizeof(trailer))) {
        uint32_t expectedCrc = trailer[0] | (static_cast<uint32_t>(trailer[1]) << 8) |
                              (static_cast<uint32_t>(trailer[2]) << 16) |
                              (static_cast<uint32_t>(trailer[3]) << 24);
        uint32_t expectedSize = trailer[4] | (static_cast<uint32_t>(trailer[5]) << 8) |
                                (static_cast<uint32_t>(trailer[6]) << 16) |
                                (static_cast<uint32_t>(trailer[7]) << 24);
        valid = reader.atEnd() && outputCrc == expectedCrc &&
                static_cast<uint32_t>(outputSize) == expectedSize;
    } else {
        valid = false;
    }
    if (valid) {
        info.outputSize = outputSize;
        info.outputCrc = outputCrc;
    }
    heap_caps_free(decompressor);
    if (window) heap_caps_free(window);
    return valid;
}

bool hasGzipSuffix(const char *path) {
    size_t length = strlen(path);
    if (length < 3) return false;
    const char *suffix = path + length - 3;
    return suffix[0] == '.' && (suffix[1] == 'g' || suffix[1] == 'G') &&
           (suffix[2] == 'z' || suffix[2] == 'Z');
}
