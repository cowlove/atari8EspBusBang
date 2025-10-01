#pragma once
#include <inttypes.h>
#include "esp_attr.h"

struct __attribute__((packed)) PbiIocb {
    uint8_t req;
    uint8_t cmd;
    uint8_t a;
    uint8_t x;

    uint8_t y;
    uint8_t carry;
    uint8_t result;
    uint8_t psp;

    uint16_t copybuf;
    uint16_t copylen;

    uint8_t kbcode;
    uint8_t sdmctl;
    uint8_t stackprog;
    uint8_t consol;
};

IRAM_ATTR int handlePbiRequest2(PbiIocb *pbiRequest);
IRAM_ATTR void handlePbiRequest(PbiIocb *pbiRequest);

IRAM_ATTR uint8_t *mappedElseCopyIn(PbiIocb *pbiRequest, uint16_t addr, uint16_t len); 
IRAM_ATTR void dumpScreenToSerial(char tag, uint8_t *mem = NULL);
