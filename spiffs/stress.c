#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#ifdef STRESS_MEGACART
#include <errno.h>
#define STRESS_BANK_COUNT 8
#define STRESS_BANK(n) __attribute__((section(".bank" #n), noinline))
#define STRESS_FIXED __attribute__((section(".bank0_fixed"), noinline))
#define DOS_ERRNO_RESET() (errno = 0)
#define DOS_ERRNO_VALUE() (errno)
void reportDosFailure(const char *operation, int returnCode, int errorNumber) {
    printf("DOS %s failed: rc=%d errno=%d\r\n", operation, returnCode, errorNumber);
}
#else
#include <banked-call.h>
#define STRESS_BANK_COUNT 16
#define STRESS_BANK(n) ATARIMAX_BANK(n)
#define STRESS_FIXED
#define DOS_ERRNO_RESET() ((void)0)
#define DOS_ERRNO_VALUE() 0
#define reportDosFailure(operation, returnCode, errorNumber) ((void)0)
#endif


volatile uint8_t *portb = (uint8_t *)0xd301;
volatile uint8_t *cartA = (uint8_t *)0xa000;
volatile uint8_t *nmien = (uint8_t *)0xd40e;
volatile uint8_t *osC = (uint8_t *)0xc000;
volatile uint8_t *d500 = (uint8_t *)0xd500;
volatile uint8_t *_0x0600 = (uint8_t *)0x600;
volatile uint8_t *_0x4000 = (uint8_t *)0x4000;
volatile uint8_t *_0xd1ff = (uint8_t *)0xd1ff;
volatile uint8_t *sdmctl = (uint8_t *)0x022f;
volatile uint8_t *vcount = (uint8_t *)0xd40b;
volatile uint8_t *dmactl = (uint8_t *)0xd400;


#define CALLS_PER_REPORT 1800

volatile uint16_t bank_calls[STRESS_BANK_COUNT];

#ifdef STRESS_MEGACART
static uint8_t currentCartBank = 0;

STRESS_FIXED void stress_banked_call(uint8_t bank, void (*function)(void)) {
    uint8_t previousBank = currentCartBank;
    currentCartBank = bank;
    *d500 = bank;
    function();
    currentCartBank = previousBank;
    *d500 = previousBank;
}
#define CALL_FUNCTION(bank, function) stress_banked_call((bank), (function))
#else
#define CALL_FUNCTION(bank, function) atarimax_banked_call((bank), (function))
#endif

#define DEFINE_BANK_PROBE(n)                                               \
  STRESS_BANK(n) void bank##n##_probe(void) { ++bank_calls[n]; }

#define CALL_BANK(n) CALL_FUNCTION(n, bank##n##_probe)

DEFINE_BANK_PROBE(0)
DEFINE_BANK_PROBE(1)
DEFINE_BANK_PROBE(2)
DEFINE_BANK_PROBE(3)
DEFINE_BANK_PROBE(4)
DEFINE_BANK_PROBE(5)
DEFINE_BANK_PROBE(6)
DEFINE_BANK_PROBE(7)
#if STRESS_BANK_COUNT > 8
DEFINE_BANK_PROBE(8)
DEFINE_BANK_PROBE(9)
DEFINE_BANK_PROBE(10)
DEFINE_BANK_PROBE(11)
DEFINE_BANK_PROBE(12)
DEFINE_BANK_PROBE(13)
DEFINE_BANK_PROBE(14)
DEFINE_BANK_PROBE(15)
#endif

long globalErrors = 0;

long cioErrors = 0;
STRESS_FIXED void resetWdt() {
    *_0x0600 = 0xde;
}

void testCio(int count) { 
    // THEORY: running a display list from unmapped gibberish memory is somehow disturbing mmu or core1 state
    uint8_t oldsdm = *sdmctl;
    *sdmctl = *dmactl = 0;
    while(*vcount != 0) {}

    while(count-- > 0) { 
        int fd = open("J1:DUMPSCREEN", O_CREAT | O_WRONLY);
        if( fd > 0) {
            close(fd);
        } else { 
            cioErrors++;
        }
        resetWdt();
    }
    *sdmctl = *dmactl = oldsdm;
}

long dosErrors = 0;
int copyFile(const char *sourcePath, const char *destPath) {
    uint8_t buffer[32];
    DOS_ERRNO_RESET();
    int source = open(sourcePath, O_RDONLY);
    if (source < 0) {
        int errorNumber = DOS_ERRNO_VALUE();
        reportDosFailure("copy open source", source, errorNumber);
        return 0;
    }

    DOS_ERRNO_RESET();
    int dest = open(destPath, O_CREAT | O_WRONLY | O_TRUNC);
    if (dest < 0) {
        int errorNumber = DOS_ERRNO_VALUE();
        reportDosFailure("copy open destination", dest, errorNumber);
        DOS_ERRNO_RESET();
        int closeResult = close(source);
        if (closeResult != 0) reportDosFailure("copy close source", closeResult, DOS_ERRNO_VALUE());
        return 0;
    }

    int success = 1;
    int bytesRead;
    for (;;) {
        DOS_ERRNO_RESET();
        bytesRead = read(source, buffer, sizeof(buffer));
        if (bytesRead <= 0) break;
        DOS_ERRNO_RESET();
        int bytesWritten = write(dest, buffer, bytesRead);
        if (bytesWritten != bytesRead) {
            reportDosFailure("copy write", bytesWritten, DOS_ERRNO_VALUE());
            success = 0;
            break;
        }
    }
    if (bytesRead < 0) {
        reportDosFailure("copy read", bytesRead, DOS_ERRNO_VALUE());
        success = 0;
    }
    DOS_ERRNO_RESET();
    int closeSourceResult = close(source);
    if (closeSourceResult != 0) {
        reportDosFailure("copy close source", closeSourceResult, DOS_ERRNO_VALUE());
        success = 0;
    }
    DOS_ERRNO_RESET();
    int closeDestResult = close(dest);
    if (closeDestResult != 0) {
        reportDosFailure("copy close destination", closeDestResult, DOS_ERRNO_VALUE());
        success = 0;
    }
    return success;
}

int filesEqual(const char *firstPath, const char *secondPath) {
    uint8_t firstByte;
    uint8_t secondByte;
    DOS_ERRNO_RESET();
    int first = open(firstPath, O_RDONLY);
    if (first < 0) {
        reportDosFailure("compare open first", first, DOS_ERRNO_VALUE());
        return 0;
    }

    DOS_ERRNO_RESET();
    int second = open(secondPath, O_RDONLY);
    if (second < 0) {
        reportDosFailure("compare open second", second, DOS_ERRNO_VALUE());
        DOS_ERRNO_RESET();
        int closeResult = close(first);
        if (closeResult != 0) reportDosFailure("compare close first", closeResult, DOS_ERRNO_VALUE());
        return 0;
    }

    int equal = 1;
    for (;;) {
        DOS_ERRNO_RESET();
        int firstCount = read(first, &firstByte, 1);
        int firstErrno = DOS_ERRNO_VALUE();
        DOS_ERRNO_RESET();
        int secondCount = read(second, &secondByte, 1);
        int secondErrno = DOS_ERRNO_VALUE();
        if (firstCount < 0) {
            reportDosFailure("compare read first", firstCount, firstErrno);
            equal = 0;
        }
        if (secondCount < 0) {
            reportDosFailure("compare read second", secondCount, secondErrno);
            equal = 0;
        }
        if (!equal) {
            break;
        }
        if (firstCount != secondCount || (firstCount > 0 && firstByte != secondByte)) {
            reportDosFailure("compare contents", 0, 0);
            equal = 0;
            break;
        }
        if (firstCount == 0) break;
    }
    DOS_ERRNO_RESET();
    int closeFirstResult = close(first);
    if (closeFirstResult != 0) {
        reportDosFailure("compare close first", closeFirstResult, DOS_ERRNO_VALUE());
        equal = 0;
    }
    DOS_ERRNO_RESET();
    int closeSecondResult = close(second);
    if (closeSecondResult != 0) {
        reportDosFailure("compare close second", closeSecondResult, DOS_ERRNO_VALUE());
        equal = 0;
    }
    return equal;
}

void testDos(int count) {
    static const uint8_t testData[] = "STRESS DOS DISK I/O TEST\r\n0123456789\r\n";
    const char *sourcePath = "D1:TESTFILE.TXT";
    const char *copy1Path = "D1:COPY1.TXT";
    const char *copy2Path = "D1:COPY2.TXT";

    while (count-- > 0) {
        DOS_ERRNO_RESET();
        int testFile = open(sourcePath, O_CREAT | O_WRONLY | O_TRUNC);
        if (testFile < 0) {
            reportDosFailure("test open", testFile, DOS_ERRNO_VALUE());
            dosErrors++;
        } else {
            DOS_ERRNO_RESET();
            int writeResult = write(testFile, testData, sizeof(testData) - 1);
            if (writeResult != sizeof(testData) - 1) {
                reportDosFailure("test write", writeResult, DOS_ERRNO_VALUE());
                dosErrors++;
            }
            DOS_ERRNO_RESET();
            int closeResult = close(testFile);
            if (closeResult != 0) {
                reportDosFailure("test close", closeResult, DOS_ERRNO_VALUE());
                dosErrors++;
            }

            if (!copyFile(sourcePath, copy1Path)) dosErrors++;
            if (!copyFile(sourcePath, copy2Path)) dosErrors++;
            if (!filesEqual(copy1Path, copy2Path)) dosErrors++;
        }
        resetWdt();
    }
}

long cartBankErrors = 0;
STRESS_BANK(1) void testCartBank(int totalCalls) {
    for (uint8_t bank = 0; bank < STRESS_BANK_COUNT; ++bank) {
      bank_calls[bank] = 0;
    }
    for (uint16_t call = 0; call < totalCalls; ++call) {
      CALL_BANK(0);
      CALL_BANK(1);
      CALL_BANK(2);
      CALL_BANK(3);
      CALL_BANK(4);
      CALL_BANK(5);
      CALL_BANK(6);
      CALL_BANK(7);
#if STRESS_BANK_COUNT > 8
      CALL_BANK(8);
      CALL_BANK(9);
      CALL_BANK(10);
      CALL_BANK(11);
      CALL_BANK(12);
      CALL_BANK(13);
      CALL_BANK(14);
      CALL_BANK(15);
#endif
      resetWdt();
    }

    for (uint8_t bank = 0; bank < STRESS_BANK_COUNT; ++bank) {
      if (bank_calls[bank] != totalCalls) {
        ++cartBankErrors;
      }
    }
}

STRESS_BANK(1) void testCartBankBanked(void) {
    testCartBank(CALLS_PER_REPORT);
}

long osErrors = 0;
STRESS_BANK(1) void testOs(int count) {
    uint8_t origC000 = *osC;
    // TODO - antic still tries to access the default character set and puts garbage on the screen
    // need to copy the default character set someplace else

        for (long n = 0; n < count; n++) {
            uint8_t osC000 = *osC;
            __asm("sei");
            *nmien = 0;
            *portb = 0xfe; // turn OS off
            uint8_t noC000 = *osC;
            if (n > 0 && noC000 != 0xee) osErrors++;
            *osC = 0xee;
            *portb = 0xff; // turn OS on
            __asm("cli");
            *nmien = 192;
            *osC = 0x11; // ROM write should be ignored
            osC000 = *osC;
            if (osC000 != origC000) osErrors++;
            //for(int delay = 0; delay < 10000; delay++) {} // add delay for 130XE with no READY halt
            resetWdt();
        }
}

STRESS_BANK(1) void testOsBanked(void) {
    testOs(1000);
}

long memErrors = 0;
void testExtMem(int count, int v) { 
    while(count-- > 0) { 
        if (v) printf("extmem:\n");
        for(int i = 0; i < 32; i++) {
                if (v) printf("%02d ", i);
                if (v && (i & 7) == 7) printf("\n");
                *portb = 0x83 | (i << 2);
                *_0x4000 = i;
                *portb = 0xff;
        }
        *_0x4000 = 0xff;
        if (v) printf("\n");
        for(int i = 0; i < 32; i++) {
                *portb = 0x83 | (i << 2);
                uint8_t expected, got = *_0x4000;
                *portb = 0xff;

                // Atari 130XE bank switch  
                //expected = 0x18 | (i & 0x3);
                //if ((i & 0x4) != 0) expected = 0xff;
               
                // Rambo256 bank layout 
                expected = (i & 0x1f);
                if ((i & 0x4) != 0 || i < 4) expected = 0xff;
                
                // Stock XL with no banks
                //expected = 0xff;

                if (got != expected) memErrors++;
                if (v) { 
                    //printf("extmem test %d  %02x %02x\n", i, expected, got);
                    printf("%02x:%02x=%02x ", i, expected, got);
                    if ((i & 0x3) == 0x3) printf("\n");
                }
        }
        if (v) printf("\n");
    }
}

// copy the character map into ram underlaying the OS ROM so that the text screen
// doesn't glitch when OS ROM is swapped out 
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


int mainNO() { 
    testExtMem(1,1);
    while(1) {
        printf("HELLO ");
        resetWdt();
    }
}

int main() { 
    //*sdmctl = *dmactl = 0;
    printf("\n\n\nS T R E S S\n\n");
    copyCharMap();
    long loopCount = 0;
    testExtMem(1, 1);
    while(1) { 
        CALL_FUNCTION(1, testCartBankBanked);
        testCio(300);
        testDos(1);
        CALL_FUNCTION(1, testOsBanked);
        testExtMem(100, 0);
        globalErrors = cioErrors + cartBankErrors + osErrors + memErrors + dosErrors;
        printf("%ld: cio:%ld cart:%ld os:%ld ext:%ld dos:%ld err:%ld\n",
                loopCount, cioErrors, cartBankErrors, osErrors, memErrors, dosErrors, globalErrors);
        loopCount++;
    }
}
