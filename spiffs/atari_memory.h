#ifndef STRESS_ATARI_MEMORY_H
#define STRESS_ATARI_MEMORY_H

#include <stdint.h>

#define ATARI_PORTB (*(volatile uint8_t *)0xd301)
#define ATARI_CART_A (*(volatile uint8_t *)0xa000)
#define ATARI_NMIEN (*(volatile uint8_t *)0xd40e)
#define ATARI_OS_ROM (*(volatile uint8_t *)0xc000)
#define ATARI_D500 (*(volatile uint8_t *)0xd500)
#define ATARI_WATCHDOG (*(volatile uint8_t *)0x0600)
#define ATARI_EXTENDED_RAM (*(volatile uint8_t *)0x4000)
#define ATARI_NEWPORT (*(volatile uint8_t *)0xd1ff)
#define ATARI_SDMCTL (*(volatile uint8_t *)0x022f)
#define ATARI_VCOUNT (*(volatile uint8_t *)0xd40b)
#define ATARI_DMACTL (*(volatile uint8_t *)0xd400)
#define ATARI_RAM_UNDER_OS 0x4000
#define ATARI_OS_ROM_BASE 0xe000
#define ATARI_CHARMAP_SIZE 0x03ff

#endif
