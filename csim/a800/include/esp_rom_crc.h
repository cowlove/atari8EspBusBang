#pragma once
// CSIM: the ROM's little-endian CRC32, as zlib computes it (same polynomial
// and conventions: esp_rom_crc32_le(0, ...) == crc32(0, ...)).
#include <stddef.h>
#include <stdint.h>
#include <zlib.h>
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    return (uint32_t)crc32(crc, buf, len);
}
