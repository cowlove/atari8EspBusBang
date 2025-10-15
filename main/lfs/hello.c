#include <stdio.h>
#include <stdint.h>
volatile uint8_t *portb = (uint8_t *)0xd301;
volatile uint8_t *cartA = (uint8_t *)0xa000;

int main(void) {
  while(1) { 
	FILE *f = fopen("D1:LLVMOUT.TXT", "w");
  	fprintf(f, "TEST");
  	fclose(f);

	*portb = 0xff; // TURN OFF BASIC
	*cartA = 0xee;

	uint8_t oldPortb = *portb, basicA000, nobasA000;
	for (long n = 0; n < 1000; n++) { 
		basicA000 = *cartA;
		*portb = 0xff; // TURN OFF BASIC
		nobasA000 = *cartA;
		if (nobasA000 != 0xee) { printf("0xa000 != 0xee!!!\n"); }
		*cartA = 0xee;
		*portb = 0xfd; // TURN ON BASIC
		*cartA = 0x11; // mem write should be ignored

	}
    printf("oldPortB = %02x A000 = %02x/%02x\n", oldPortb, basicA000, nobasA000);
  }
  return 0;
}
