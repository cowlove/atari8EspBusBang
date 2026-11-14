#pragma once
// CSIM: the ESP32 linked-list DMA descriptor (soc/lldesc.h). Word 0 holds
// size, length (bits 12-23: the field the RX DMA writes back as it
// completes a descriptor) and the flags; the emulated DMA (csim_i2s.cpp)
// writes it as the S3's GDMA does.
#include <stdint.h>

typedef struct lldesc_s {
    volatile uint32_t size : 12, length : 12, offset : 5, sosf : 1, eof : 1, owner : 1;
    volatile const uint8_t *buf;
    union {
        volatile uint32_t empty;
        struct lldesc_s *next;
    };
} lldesc_t;
