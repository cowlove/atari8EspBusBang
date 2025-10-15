#include <stdio.h>
#include <stdint.h>
volatile uint8_t *portb = (uint8_t *)0xd301;
volatile uint8_t *cartA = (uint8_t *)0xa000;
volatile uint8_t *nmien = (uint8_t *)0xd40e;
volatile uint8_t *osC = (uint8_t *)0xc000;
volatile uint8_t *d500 = (uint8_t *)0xd500;
int main(void) {
  long loopCount = 0;
  while(1) {
	FILE *f = fopen("D1:LLVMOUT.TXT", "w");
  	fprintf(f, "TEST");
  	fclose(f);
	*(d500 + 0x10) = 0; // switch off SDX cart

	*portb = 0xff; // TURN OFF BASIC
	*cartA = 0xee;

	uint8_t oldPortb = *portb, basicA000, nobasA000, osC000, noC000;
	printf("Testing BASIC on/off...\n");
	fflush(stdout);
	for (long n = 0; n < 10000; n++) { 
		basicA000 = *cartA;
		*portb = 0xff; // switch basic off
		nobasA000 = *cartA;
		if (nobasA000 != 0xee) { printf("0xa000 != 0xee!!!\n"); }
		*cartA = 0xee;
		*portb = 0xfd; // switch basic on
		*cartA = 0x11; // ROM write should be ignored

	}
	printf("Testing OS on/off...\n");
	fflush(stdout);
	for (long n = 0; n < 10000; n++) {
		osC000 = *osC;
		__asm("sei");
		*nmien = 0;
		*portb = 0xfe; // turn OS off
		noC000 = *osC;
		*osC = 0xee;
		*portb = 0xff; // turn OS on 
		*osC = 0x11; // ROM write should be ignored
		osC000 = *osC;
		for(int delay = 0; delay < 1000; delay++) {} // add delay for 130XE with no READY halt 
		__asm("cli"); 
		*nmien = 192;
	}

    printf("%06ld A000 = %02x/%02x C000 = %02x/%02x\n", 
		loopCount++, basicA000, nobasA000, osC000, noC000);
	fflush(stdout);
  }
  return 0;
}
