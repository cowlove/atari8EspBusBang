#pragma once

#include <stddef.h>
#include <stdint.h>

#include "spiffs.h"

struct GzipImageInfo {
    size_t outputSize = 0;
    uint32_t outputCrc = 0;
    uint8_t prefix[16] = {};
};

bool gzipImageInflate(spiffs *fs, spiffs_file fd, size_t fileSize, uint8_t *output,
                      size_t outputCapacity, size_t maxOutputSize, GzipImageInfo &info);
bool hasGzipSuffix(const char *path);
