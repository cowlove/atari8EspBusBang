#ifndef STRESS_ATARI_MEMORY_H
#define STRESS_ATARI_MEMORY_H

#include <stdint.h>

static volatile uint8_t * const portb        = (volatile uint8_t *)0xd301;
static volatile uint8_t * const carta        = (volatile uint8_t *)0xa000;
static volatile uint8_t * const nmien        = (volatile uint8_t *)0xd40e;
static volatile uint8_t * const osrom        = (volatile uint8_t *)0xc000;
static volatile uint8_t * const d500         = (volatile uint8_t *)0xd500;
static volatile uint8_t * const watchdog     = (volatile uint8_t *)0x0600;
static volatile uint8_t * const extended_ram = (volatile uint8_t *)0x7000;
static volatile uint8_t * const newport      = (volatile uint8_t *)0xd1ff;
static volatile uint8_t * const sdmctl       = (volatile uint8_t *)0x022f;
static volatile uint8_t * const vcount       = (volatile uint8_t *)0xd40b;
static volatile uint8_t * const dmactl       = (volatile uint8_t *)0xd400;

#define ram_under_os 0x4000
#define os_rom_base 0xe000
#define charmap_size 0x03ff

#endif
