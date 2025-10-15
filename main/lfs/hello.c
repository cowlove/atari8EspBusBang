#include <stdio.h>
#include <stdint.h>
uint8_t *portb = (uint8_t *)0xd301;
uint8_t *cartA = (uint8_t *)0xa000;

int main(void) {
  while(1) { 
	FILE *f = fopen("D1:LLVMOUT.TXT", "w");
  	fprintf(f, "TEST");
  	fclose(f);
	uint8_t oldPortb = *portb;
	uint8_t basicA000 = *cartA;
	*portb = 0xff;
	uint8_t nobasA000 = *cartA;
	*portb = oldPortb;
        printf("oldPortB = %02x A000 = %02x/%02x\n", oldPortb, basicA000, nobasA000);
  }
  return 0;
}
