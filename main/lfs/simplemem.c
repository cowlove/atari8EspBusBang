#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h> 

volatile uint8_t *portb = (uint8_t *)0xd301;
volatile uint8_t *cartA = (uint8_t *)0xa000;
volatile uint8_t *nmien = (uint8_t *)0xd40e;
volatile uint8_t *osC = (uint8_t *)0xc000;
volatile uint8_t *d500 = (uint8_t *)0xd500;
volatile uint8_t *_0x0600 = (uint8_t *)0x600;
volatile uint8_t *_0x4000 = (uint8_t *)0x4000;
volatile uint8_t *_0xd1ff = (uint8_t *)0xd1ff;

volatile uint8_t *sdmctl = (uint8_t *)0x22f;

int main(void) { 
	int count = 0;
    while(1) { 
        for(long i = 0; i < 100000; i++) 
            *_0x0600 = 0xde;

        printf("% d", count++);
    }
}
