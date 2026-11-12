#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>

struct GzipImageInfo {
    size_t outputSize = 0;
    uint32_t outputCrc = 0;
    uint8_t prefix[16] = {};
};

// Reads up to len bytes of the gzip file into buf, from where the last call
// left off: the byte count, or <= 0 at the end or on an error.
using GzipReadFn = std::function<int(uint8_t *buf, size_t len)>;

// Inflate the fileSize-byte gzip file that read() returns, from its start.
bool gzipImageInflate(const GzipReadFn &read, size_t fileSize, uint8_t *output,
                      size_t outputCapacity, size_t maxOutputSize, GzipImageInfo &info);
bool hasGzipSuffix(const char *path);
