#pragma once
#include <stdint.h>
#include "esp_attr.h"
#include "mmu.h"
#include "asmDefs.h"
#include "pinDefs.h"

const static DRAM_ATTR uint16_t _0x1ff = 0x1ff;
const static DRAM_ATTR uint16_t _0x301 = 0x301;
const static DRAM_ATTR uint16_t _0x4000 = 0x4000;
const static DRAM_ATTR uint16_t _0x7fff = 0x7fff;
const static DRAM_ATTR uint16_t _0xffff = 0xffff;
const static DRAM_ATTR uint16_t _0xc000 = 0xc000;
const static DRAM_ATTR uint16_t _0xcfff = 0xcfff;
const static DRAM_ATTR uint16_t _0x5000 = 0x5000;
const static DRAM_ATTR uint16_t _0x57ff = 0x57ff;
const static DRAM_ATTR uint16_t _0xe000 = 0xe000;
const static DRAM_ATTR uint16_t _0xa000 = 0xa000;
const static DRAM_ATTR uint16_t _0xbfff = 0xbfff;
const static DRAM_ATTR uint16_t _0xd800 = 0xd800;
const static DRAM_ATTR uint16_t _0xdfff = 0xdfff;
const static DRAM_ATTR uint16_t _0x8000 = 0x8000;
const static DRAM_ATTR uint16_t _0x9fff = 0x9fff;
static const DRAM_ATTR uint32_t _0xffffffff = ~0;

static const DRAM_ATTR uint16_t _0xd830 = 0xd830;
static const DRAM_ATTR uint16_t _0xd840 = 0xd840;
static const DRAM_ATTR uint16_t _0xd301 = 0xd301;
static const DRAM_ATTR uint16_t _0xd1ff = 0xd1ff;
static const DRAM_ATTR uint16_t _0xd500 = 0xd500;
static const DRAM_ATTR uint16_t _0xd300 = 0xd300;
static const DRAM_ATTR uint16_t _0xff00 = 0xff00;
static const DRAM_ATTR uint16_t pageNr_d301 = pageNr(0xd301);
static const DRAM_ATTR uint16_t pageNr_d1ff = pageNr(0xd1ff);
static const DRAM_ATTR uint16_t pageNr_d500 = pageNr(0xd500);

static const DRAM_ATTR int pbiDeviceNumMask = PDEVNUM;
static const DRAM_ATTR int pbiDeviceNumShift = 1;
static const DRAM_ATTR uint32_t interruptMaskNOT = ~bus.irq_.mask;
static const DRAM_ATTR uint32_t pbiDeviceNumMaskNOT = ~pbiDeviceNumMask;

static const  DRAM_ATTR int PDIMSK = 0x249;

