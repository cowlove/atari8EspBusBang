#pragma once
#include <stdint.h>
#include "esp_attr.h"
#include "asmDefs.h"
#include "pinDefs.h"

#ifndef DRAM_ATTR
#define DRAM_ATTR
#error 
#endif 

constexpr static DRAM_ATTR uint16_t _0x1ff = 0x1ff;
constexpr static DRAM_ATTR uint16_t _0x301 = 0x301;
constexpr static DRAM_ATTR uint16_t _0x4000 = 0x4000;
constexpr static DRAM_ATTR uint16_t _0x7fff = 0x7fff;
constexpr static DRAM_ATTR uint16_t _0xffff = 0xffff;
constexpr static DRAM_ATTR uint16_t _0xc000 = 0xc000;
constexpr static DRAM_ATTR uint16_t _0xcfff = 0xcfff;
constexpr static DRAM_ATTR uint16_t _0x5000 = 0x5000;
constexpr static DRAM_ATTR uint16_t _0x57ff = 0x57ff;
constexpr static DRAM_ATTR uint16_t _0xe000 = 0xe000;
constexpr static DRAM_ATTR uint16_t _0xa000 = 0xa000;
constexpr static DRAM_ATTR uint16_t _0xbfff = 0xbfff;
constexpr static DRAM_ATTR uint16_t _0xd800 = 0xd800;
constexpr static DRAM_ATTR uint16_t _0xdfff = 0xdfff;
constexpr static DRAM_ATTR uint16_t _0x8000 = 0x8000;
constexpr static DRAM_ATTR uint16_t _0x9fff = 0x9fff;
static constexpr DRAM_ATTR uint32_t _0xffffffff = ~0;

static constexpr DRAM_ATTR uint16_t _0xd830 = 0xd830;
static constexpr DRAM_ATTR uint16_t _0xd840 = 0xd840;
static constexpr DRAM_ATTR uint16_t _0xd301 = 0xd301;
static constexpr DRAM_ATTR uint16_t _0xd1ff = 0xd1ff;
static constexpr DRAM_ATTR uint16_t _0xd500 = 0xd500;
static constexpr DRAM_ATTR uint16_t _0xd300 = 0xd300;
static constexpr DRAM_ATTR uint16_t _0xff00 = 0xff00;

static constexpr DRAM_ATTR int pbiDeviceNumMask = PDEVNUM;
static constexpr DRAM_ATTR int pbiDeviceNumShift = 1;
static constexpr DRAM_ATTR uint32_t interruptMaskNOT = ~bus.irq_.mask;
static constexpr DRAM_ATTR uint32_t haltMaskNOT = ~bus.halt_.mask; 
static constexpr DRAM_ATTR uint32_t pbiDeviceNumMaskNOT = ~pbiDeviceNumMask;

static constexpr  DRAM_ATTR int PDIMSK = 0x249;
