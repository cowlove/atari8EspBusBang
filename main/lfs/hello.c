#include <stdio.h>

int main(void) {
  while(1) { 
	FILE *f = fopen("D1:LLVMOUT.TXT", "w");
  	fprintf(f, "TEST");
  	fclose(f);
  	printf("HELLO, %s!\n", "PRINTF");
  }
  return 0;
}
