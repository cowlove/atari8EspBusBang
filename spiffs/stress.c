#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "cartridge.h"

CART_DEFINE_BANK_PROBE(0)
CART_DEFINE_BANK_PROBE(1)
CART_DEFINE_BANK_PROBE(2)
CART_DEFINE_BANK_PROBE(3)
CART_DEFINE_BANK_PROBE(4)
CART_DEFINE_BANK_PROBE(5)
CART_DEFINE_BANK_PROBE(6)
CART_DEFINE_BANK_PROBE(7)
#if CART_BANK_COUNT > 8
CART_DEFINE_BANK_PROBE(8)
CART_DEFINE_BANK_PROBE(9)
CART_DEFINE_BANK_PROBE(10)
CART_DEFINE_BANK_PROBE(11)
CART_DEFINE_BANK_PROBE(12)
CART_DEFINE_BANK_PROBE(13)
CART_DEFINE_BANK_PROBE(14)
CART_DEFINE_BANK_PROBE(15)
#endif

long cioErrors = 0;
CART_FIXED void resetWdt() {
    *watchdog = 0xde;
}

// this seems very marginal, some bankStress() calls failed with 199
// #define ASM_DELAY() "sta $e000 \n nop\n nop\n" //nop\nnop\nnop\nnop\n"

//#define ASM_DELAY() "inc $81\n nop\n"
//sta $e000 \n nop\n nop\n" //nop\nnop\nnop\nnop\n"
//#define ASM_DELAY() "nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
#define ASM_DELAY() ""
#define DELAY() delay()

static inline void delay() { 
     __attribute__((leaf)) asm volatile(
       "nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
     );
}

static volatile uint8_t remaining;
#define TEST_LOCATION "$7000"


uint8_t bankStress(uint8_t count) {
    __attribute__((leaf)) asm volatile(

        // TODO: write loop count into "remaining" global
        "sta %1     \n\t"
        "ldy #$e7   \n\t"  // portb mask for bank 2

        // IDEA - switch as fast as possible between two banks, in a single byte
        // read/verify sequence.  Pre-load data values for bank 1 in X and bank 2 in Y. 
        // Read them back into X and Y for comparison after the switching to ensure the 
        // fastest possible bank switching sequence.  
".LOOP%=:\n\t"
        "ldx #$e3   \n\t"  // portb mask for bank 1
        "lda #$01   \n\t"  
        "stx $d301  \n\t"  // switch to bank 1 and write #1
        ASM_DELAY() 
        "sta "TEST_LOCATION"  \n\t"  
        "sty $d301  \n\t"  // switch to bank 2 and write #2
        ASM_DELAY() 
        "eor #$03   \n\t"
        "sta "TEST_LOCATION"  \n\t"  
        "stx $d301  \n\t"  // load bank 1 value into A
        ASM_DELAY() 
        "lda "TEST_LOCATION" \n\t"
        "sty $d301  \n\t"  // load bank 2 value into X
        ASM_DELAY() 
        "ldx "TEST_LOCATION"  \n\t"

        // compare A and X with expected values, branch to ERR1 and exit the function on fail 
        "cmp #$01       \n\t"
        "bne .RETURN%=    \n\t"
        "txa            \n\t"
        "cmp #$02       \n\t"
        "bne .RETURN%=    \n\t"

        // Round 2, same thing but writing values of 5 and 6
        "ldx #$e3   \n\t"
//        "ldy #$87   \n\t"  // unchanged from being set above 
        "lda #$05   \n\t"  
        "stx $d301  \n\t"  // switch to bank 1 and write #5 
        ASM_DELAY() 
        "sta "TEST_LOCATION" \n\t"  
        "sty $d301  \n\t"  // switch to bank 2 and write #6
        ASM_DELAY() 
        "eor #$03   \n\t"
        "sta "TEST_LOCATION"  \n\t"  
        "stx $d301  \n\t"  // load bank 1 value into A
        ASM_DELAY() 
        "lda "TEST_LOCATION"  \n\t"
        "sty $d301  \n\t"  // load bank 2 value into X
        ASM_DELAY() 
        "ldx "TEST_LOCATION"  \n\t"

        // compare A and X with expected values, branch to ERR1 and exit the function on fail 
        "cmp #$05       \n\t"
        "bne .RETURN%=    \n\t"
        "txa            \n\t"
        "cmp #$06       \n\t"
        "bne .RETURN%=    \n\t"

        // loop using global memory location
        "dec %1         \n\t"
        "beq .RETURN%=  \n\t"
        "jmp .LOOP%=    \n\t"
".RETURN%=:             \n\t"
        "lda #$ff       \n\t"  // restore portb 
        "sta $d301      \n\t"
        "lda %1         \n\t"  // return value is number if iterations remaining

        : "+a"(count), "+m"(remaining)
        :
        : "p", "memory", "x", "y"
    );
    return count /* TODO: count is now error count */;
}

void testCio(int count) { 
    // THEORY: running a display list from unmapped gibberish memory is somehow disturbing mmu or core1 state
    uint8_t oldsdm = *sdmctl;
    *sdmctl = *dmactl = 0;
    while(*vcount != 0) {}

    while(count-- > 0) { 
        int fd = open("J1:WDRESET", O_CREAT | O_WRONLY);
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
    int source = open(sourcePath, O_RDONLY);
    if (source < 0) return 0;

    int dest = open(destPath, O_CREAT | O_WRONLY | O_TRUNC);
    if (dest < 0) {
        close(source);
        return 0;
    }

    int success = 1;
    int bytesRead;
    for (;;) {
        bytesRead = read(source, buffer, sizeof(buffer));
        if (bytesRead < 0) {
            success = 0;
            break;
        }
        if (bytesRead == 0) break;
        int bytesWritten = write(dest, buffer, bytesRead);
        if (bytesWritten != bytesRead) {
            success = 0;
            break;
        }
    }
    if (close(source) != 0) success = 0;
    if (close(dest) != 0) success = 0;
    return success;
}

int filesEqual(const char *firstPath, const char *secondPath) {
    uint8_t firstByte;
    uint8_t secondByte;
    int first = open(firstPath, O_RDONLY);
    if (first < 0) return 0;

    int second = open(secondPath, O_RDONLY);
    if (second < 0) {
        close(first);
        return 0;
    }

    int equal = 1;
    for (;;) {
        int firstCount = read(first, &firstByte, 1);
        int secondCount = read(second, &secondByte, 1);
        if (firstCount < 0 || secondCount < 0) {
            equal = 0;
            break;
        }
        if (firstCount != secondCount || (firstCount > 0 && firstByte != secondByte)) {
            equal = 0;
            break;
        }
        if (firstCount == 0) break;
    }
    if (close(first) != 0) equal = 0;
    if (close(second) != 0) equal = 0;
    return equal;
}

void testDos(int count) {
    static const uint8_t testData[] = "STRESS DOS DISK I/O TEST\r\n0123456789\r\n";
    const char *sourcePath = "D1:TESTFILE.TXT";
    const char *copy1Path = "D1:COPY1.TXT";
    const char *copy2Path = "D1:COPY2.TXT";

    while (count-- > 0) {
        int testFile = open(sourcePath, O_CREAT | O_WRONLY | O_TRUNC);
        if (testFile < 0) {
            dosErrors++;
            return;
        }
        int writeResult = write(testFile, testData, sizeof(testData) - 1);
        int closeResult = close(testFile);
        if (writeResult != sizeof(testData) - 1 || closeResult != 0) {
            dosErrors++;
            return;
        }
        if (!copyFile(sourcePath, copy1Path)) {
            dosErrors++;
            return;
        }
        if (!copyFile(sourcePath, copy2Path)) {
            dosErrors++;
            return;
        }
        if (!filesEqual(copy1Path, copy2Path)) {
            dosErrors++;
            return;
        }
        resetWdt();
    }
}

long cartBankErrors = 0;
CART_BANK(1) void testCartBank(int totalCalls) {
    for (uint8_t bank = 0; bank < CART_BANK_COUNT; ++bank) {
      cartBankCalls[bank] = 0;
    }
    for (uint16_t call = 0; call < totalCalls; ++call) {
      CART_CALL_BANK(0);
      CART_CALL_BANK(1);
      CART_CALL_BANK(2);
      CART_CALL_BANK(3);
      CART_CALL_BANK(4);
      CART_CALL_BANK(5);
      CART_CALL_BANK(6);
      CART_CALL_BANK(7);
#if CART_BANK_COUNT > 8
      CART_CALL_BANK(8);
      CART_CALL_BANK(9);
      CART_CALL_BANK(10);
      CART_CALL_BANK(11);
      CART_CALL_BANK(12);
      CART_CALL_BANK(13);
      CART_CALL_BANK(14);
      CART_CALL_BANK(15);
#endif
      resetWdt();
    }

    for (uint8_t bank = 0; bank < CART_BANK_COUNT; ++bank) {
      if (cartBankCalls[bank] != totalCalls) {
        ++cartBankErrors;
      }
    }
}

CART_BANK(1) void testCartBankBanked(void) {
    testCartBank(CART_CALLS_PER_REPORT);
}

long osErrors = 0;
CART_BANK(1) void testOs(int count) {
    uint8_t origC000 = *osrom;
    // TODO - antic still tries to access the default character set and puts garbage on the screen
    // need to copy the default character set someplace else

        for (long n = 0; n < count; n++) {
            uint8_t osC000 = *osrom;
            __asm("sei");
            *nmien = 0;
            *portb = 0xfe; // turn OS off
            uint8_t noC000 = *osrom;
            if (n > 0 && noC000 != 0xee) osErrors++;
            *osrom = 0xee;
            *portb = 0xff; // turn OS on
            __asm("cli");
            *nmien = 192;
            *osrom = 0x11; // ROM write should be ignored
            osC000 = *osrom;
            if (osC000 != origC000) osErrors++;
            //for(int delay = 0; delay < 10000; delay++) {} // add delay for 130XE with no READY halt
            resetWdt();
        }
}

CART_BANK(1) void testOsBanked(void) {
    testOs(1000);
}

struct {
    uint8_t map[32];
    const char *name;
} layouts[] = { {{
    0x00,0x01,0x02,0x03,                  
    0xba,0xba,0xba,0xba,                  
    0x00,0x01,0x02,0x03,                  
    0xba,0xba,0xba,0xba,                  
    0x00,0x01,0x02,0x03,                  
    0xba,0xba,0xba,0xba,                  
    0x00,0x01,0x02,0x03,                  
    0xba,0xba,0xba,0xba,                  
}, "XE"}, {{
    0x00,0x01,0x02,0x03,                  
    0xba,0xba,0xba,0xba,                  
    0x08,0x09,0x0a,0x0b,                  
    0xba,0xba,0xba,0xba,                  
    0x10,0x11,0x12,0x13,                  
    0xba,0xba,0xba,0xba,                  
    0x18,0x19,0x1a,0x1b,                  
    0xba,0xba,0xba,0xba,                  
}, "RAMBO256"}, {{
    0xba, 0xba, 0xba, 0xba,
    0xba, 0xba, 0xba, 0xba,
    0x08, 0x09, 0x0a, 0x0b,
    0xba, 0xba, 0xba, 0xba,
    0x10, 0x11, 0x12, 0x13,
    0xba, 0xba, 0xba, 0xba,
    0x18, 0x19, 0x1a, 0x1b,
    0xba, 0xba, 0xba, 0xba,
}, "RAMBO192"},
};

long memErrors = 0;
uint8_t observedMap[32] = {};
int8_t bankType = -1;

//CART_BANK(1) 
void probeExtMem() { 
    //printf("Probed extMem:\n");
    for(int i = 31; i >= 0; i--) {
        *portb = 0xff;
        DELAY();
        *extended_ram = 0xba;
        *portb = 0x83 | (i << 2);
        DELAY();
        *extended_ram = i;
    }
    *portb = 0xff;
    DELAY();
    *extended_ram = 0xba;

    for(uint8_t i = 0; i < 32; i++) {
        *portb = 0x83 | (i << 2);
        DELAY();
        uint8_t got = *extended_ram;;
        *portb = 0xff;
        //printf("%02x:%02x ", i, got);
        //printf("0x%02x,", got);
        //if ((i & 3) == 3) printf("\n");
        observedMap[i] = got;
    }

    for(int s = 0; s < sizeof(layouts) / sizeof(layouts[0]); s++) { 
        for(int i = 0; i < 32; i++) {
            if (layouts[s].map[i] != observedMap[i])
                break;
            if (i == 31) {
                bankType = s;
                return;
            }
        }
    }
}

void testExtMem(int count, int v) {
    if (bankType < 0) 
        return;

    uint8_t bs = bankStress(255);
    if (bs != 0) { 
        memErrors++;
        printf("bankStress() failed with %d\n", bs);

        char buf[64] = {0};
        int len = snprintf(buf, sizeof(buf), "MF1 bankStress() failed with %d", bs);
        int f2 = open("J1:LOG", O_CREAT | O_WRONLY);
        write(f2, buf, len);
        close(f2);
    } 
    while(count-- > 0) { 
        if (v) printf("extmem:\n");
        __asm("sei");
        *nmien = 0;
        for(int rewrites = 0; rewrites < 3; rewrites++) { 
            for(int i = 31; i >= 0; i--) {
                *portb = 0x83 | (i << 2);
                DELAY();
                *extended_ram = 0xee;
           }
        }
        *portb = 0xff;
        __asm("cli");
        *nmien = 192;
        for(int i = 31; i >= 0; i--) {
                //if (v) printf("%02d ", i);
                //if (v && (i & 7) == 7) printf("\n");
#define PROTECT1
#ifdef PROTECT1
                __asm("sei");
                *nmien = 0;
                *portb = 0x83 | (i << 2);
                *portb = 0x83 | (i << 2);


#endif
                *portb = 0x83 | (i << 2);
                DELAY();
                *extended_ram = i;
                *extended_ram = i;
                *extended_ram = i;
                *portb = 0xff;
#ifdef PROTECT1
                __asm("cli");
                *nmien = 192;
#endif
        }
        *extended_ram = 0xba;
        if (v) printf("\n");
        for(int i = 31; i >= 0; i--) {
                __asm("sei");
                *nmien = 0;
                *portb = 0x83 | (i << 2);
                DELAY();
                uint8_t expected, got = *extended_ram;
                __asm("cli");
                *nmien = 192;

                expected = observedMap[i];//[bankType].map[i];
                // Atari 130XE bank switch  
                //expected = 0x18 | (i & 0x3);
                //if ((i & 0x4) != 0) expected = 0xba;
               
                // Rambo256 bank layout 
                //expected = (i & 0x1f);
                //if ((i & 0x4) != 0 || i < 4) expected = 0xff;
                
                // Stock XL with no banks
                //expected = 0xff;

                if (got != expected)  { 
                    memErrors++;
                    uint8_t reread = *extended_ram;
                    *portb = 0x83 | (i << 2);
                    DELAY(); DELAY();
                    uint8_t remap = *extended_ram;
                    *portb = 0xff;
                    printf("%ld ext %02x:%02x!=%02x rerd %02x remap %02x\n", memErrors, i, got, 
                            expected, reread, remap);

                    // TODO factor this out into a log(fmt, ...) function
                    char buf[64] = {0};
                    int len = snprintf(buf, sizeof(buf), "%ld MF2 %02x:%02x!=%02x rerd %02x remap %02x\n", 
                            memErrors, i, got, expected, reread, remap);
                    int f2 = open("J1:LOG", O_CREAT | O_WRONLY);
                    write(f2, buf, len);
                    close(f2);
                }
                if (v) { 
                    printf("%02x:%02x=%02x ", i, expected, got);
                    if ((i & 0x3) == 0x3) printf("\n");
                }
                *portb = 0xff;
        }
        resetWdt();
        *portb=0xff;
        if (v) printf("\n");
    }
}

// copy the character map into ram underlaying the OS ROM so that the text screen
// doesn't glitch when OS ROM is swapped out 
void copyCharMap() { 
        memcpy((void *)ram_under_os, (void *)os_rom_base, charmap_size);

        __asm("sei");
        *nmien = 0;
        *portb = 0xfe; // turn OS off

        memcpy((void *)os_rom_base, (void *)ram_under_os, charmap_size);

        *portb = 0xff; // turn OS on
        __asm("cli"); 
        *nmien = 192;
}

#if 0 
int mainNO() { 
    printf("HELLO\n");
    while(1) {
        resetWdt();
    }
}
#endif

int main() { 
    //*sdmctl = *dmactl = 0;
    printf("\n\n\nS T R E S S\n\n");
    //copyCharMap();
    probeExtMem();
    for(int i = 0; i < 32; i++) {
        printf("0x%02x, ", observedMap[i]);
        if ((i & 3) == 3) printf("\n");
    }
    printf("Detected extended memory type %d\n", bankType);
    int f = open("J1:DUMPSCREEN", O_CREAT);
    close(f);

    long loopCount = 0;
    //testExtMem(1, 1);
    while(1) { 
        CART_CALL(1, testCartBankBanked);
        testCio(1);
        //testDos(1);
        CART_CALL(1, testOsBanked);
        testExtMem(500, 0);
        printf("%ld: cio:%ld cart:%ld os:%ld ext:%ld dos:%ld\n",
                loopCount, cioErrors, cartBankErrors, osErrors, memErrors, dosErrors);
        loopCount++;
    }
}
