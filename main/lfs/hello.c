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
volatile uint8_t *_0xd1ff = (uint8_t *)0xd1ff;

volatile uint8_t *sdmctl = (uint8_t *)0x22f;

void copyCharMap() { 
	memcpy((void *)0x4000, (void *)0xe000, 0x3ff);

	__asm("sei");
	*nmien = 0;
	*portb = 0xfe; // turn OS off

	memcpy((void *)0xe000, (void *)0x4000, 0x3ff);

	*portb = 0xff; // turn OS on 
	__asm("cli"); 
	*nmien = 192;
}

void resetWdt() { 
	int fd = open("J1:WDTIMER", O_CREAT | O_WRONLY);
    if( fd > 0) { 
	    close(fd);
	}
}
void testDisk() {
	int fd = open("D1:TEST", O_CREAT | O_WRONLY);
    if( fd > 0) { 
	    close(fd);
	}
}

long osErr = 0;
void testOs() { 
    uint8_t origC000 = *osC;
    // TODO - antic still tries to access the default character set and puts garbage on the screen
    // need to copy the default character set someplace else

   	for (long n = 0; n < 1000; n++) {
		uint8_t osC000 = *osC;
		__asm("sei");
		*nmien = 0;
		*portb = 0xfe; // turn OS off
		uint8_t noC000 = *osC;
        if (n > 0 && noC000 != 0xee) osErr++;
		*osC = 0xee;
		*portb = 0xff; // turn OS on 
		__asm("cli"); 
		*nmien = 192;
		*osC = 0x11; // ROM write should be ignored
		osC000 = *osC;
        if (osC000 != origC000) osErr++;
		//for(int delay = 0; delay < 10000; delay++) {} // add delay for 130XE with no READY halt 
	}
}
 
#if 1 
int main(void) { 
	int count = 0;
	copyCharMap();
	//*sdmctl = 0;

	while(1) { 
		printf("hello %d oserr=%ld ", count++, osErr);
		//fflush(stdout);
		resetWdt();
		//*_0xd1ff = 0x2;
		*_0x0600 = 0xde;
		//*_0xd1ff = 0;
        testOs();
	}
	return 0;
}

#else 
int main(void) {
  long loopCount = 0;
  int basicOnErr = 0, basicOffErr = 0;
  while(1) {
	FILE *f = fopen("D1:LLVMOUT.TXT", "w");
  	fprintf(f, "TEST");
  	fclose(f);
	//*(d500 + 0x10) = 0; // switch off SDX cart

	*portb = 0xff; // TURN OFF BASIC
	*cartA = 0xee;
	*portb = 0xfd; // switch basic on

	uint8_t oldPortb = *portb, basicA000, nobasA000, osC000, noC000;
	printf("Testing BASIC on/off...\n");
	fflush(stdout);
	for (long n = 0; n < 100000; n++) { 
		basicA000 = *cartA;
		if (basicA000 != 0xa5) { 
			//printf("basic on  %02x != %02x\n", basicA000, 0xa5); 
			basicOnErr++;
		}
		*portb = 0xff; // switch basic off
		nobasA000 = *cartA;
		if (nobasA000 != 0xee) { 
			//printf("basic off %02x != %02x\n", nobasA000, 0xee); 
			basicOffErr++;
		}
		*cartA = 0xee;
		*portb = 0xfd; // switch basic on
		*cartA = 0x22; // ROM write should be ignored

	}
	printf("Testing OS on/off...\n");
	fflush(stdout);
	for (long n = 0; n < 0000; n++) {
		osC000 = *osC;
		__asm("sei");
		*nmien = 0;
		*portb = 0xfe; // turn OS off
		noC000 = *osC;
		*osC = 0xee;
		*portb = 0xff; // turn OS on 
		*osC = 0x11; // ROM write should be ignored
		osC000 = *osC;
		//for(int delay = 0; delay < 10000; delay++) {} // add delay for 130XE with no READY halt 
		__asm("cli"); 
		*nmien = 192;
	}

    printf("%06ld bOnErr=%04d boffErr=%04d\n", loopCount++, basicOnErr, basicOffErr); 
	//	loopCount++, basicA000, nobasA000, osC000, noC000);
	fflush(stdout);
	f = fopen("J1:DUMPSCREEN", "w");
  	fprintf(f, "1");
  	fclose(f);
  }
  return 0;
}
#endif

