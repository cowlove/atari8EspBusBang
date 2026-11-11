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

// ===========================================================================
// Bank tests (formerly banktest.c): does a bank switch reach the very next
// bus cycle?
//
// On the Atari, a PORTB write switches the extended-memory bank, a $D5xx
// access the cartridge bank, and a $D1FF write the PBI ROM at $D800, in time
// for the very next cycle. The board maps PORTB and $D1FF before each
// cycle's address read and the AtariMax $D5xx select in the write cycle
// itself, so those should pass. MegaCart (bank = the data written to
// $D500) is one cycle late on the board: the bank is picked up by the cycle
// after the write (core1.cpp), so a switch falling straight into the window
// misses there; one with a NOP before the window should hit.
//
// The only read that directly follows a write is the next opcode fetch, so
// each test switches with an STA in the last bytes before the banked window
// and falls through into it:
//
//   ext:  $3FFD  STA $D301          falls into $4000
//   cart: $9FFD  STA $D500+B        falls into $A000   (AtariMax)
//         LDA #B / STA $D500, then the probe byte, inside banks 2 and 3
//         (MegaCart; see btMcExact). A +1 variant puts a NOP between: its
//         opcode fetch picks up the bank, its second cycle re-reads the
//         next byte, and the probe's fetch comes after.
//   pbi:  $D7FD  STA $D1FF          falls into $D800
//
// $D7FD is PBI device space, where the board keeps 512 bytes of RAM
// ($D600-$D7FF, main/mmu.cpp); nothing else answers there. For the PBI test
// the OS ROM is off, so the deselected $D800 is RAM (bank A); the selected
// one is the board's PBI ROM, whose unused checksum bytes ($D800-$D801) hold
// the probe while it runs (bank B).
//
// Bank A has RTS at the window's first byte; bank B has INX, RTS. Each
// iteration selects A the ordinary way, then JSRs to the switch, which
// selects B. If the opcode fetch at the window sees B, X counts it; if it
// still sees A, it returns without counting. Only that first byte tells the
// banks apart: by the next read the board has caught up.
//
// The loops run from RAM (the cartridge test switches the program's own
// bank out) with IRQs and NMIs off. ANTIC DMA stays on: a DMA read between
// the STA and the fetch can let the board catch up by luck, so a miss is
// proof of a late switch, but a clean count from the board alone is not
// proof of a fix. Run with DMA off (SDMCTL) for that. Before each test, a
// calibration selects each bank with time to settle and calls the window,
// to check that the seeding took.
//
// basic (AtariMax only): disabling BASIC with STA PORTB at $9FFD falls into
// $A000, where the RAM under BASIC holds RTS: the correct path just returns.
// A late board fetches BASIC ROM's first opcode instead (Atari BASIC: LDA
// $CA; Altirra BASIC: LDX $08). By the next read it has caught up, so the
// operand and what follows come from RAM, seeded to reach JMP basicMiss,
// which counts the miss in memory and returns through the loop's JSR. Only
// the disable direction can be tested: a correct enable would run BASIC.
// The test runs from RAM with the cartridge off, and a startup probe of
// BASIC's opcode picks the seeds. Each enable is followed by a read outside
// $A000-$BFFF, so a late board never drives its RAM over the enabled ROM.
// Skipped on MegaCart: no $D500 value switches it off on both the board
// (cartBanks[$D500 & 0x1f], 8 and up map RAM) and a real cart (bit 7).
//
// Layout. AtariMax maps one 8 KB bank at $A000 at a time, so this code and
// its tables live in bank 1 (BT_CODE, BT_RODATA) and may call nothing in
// bank 0: no libc, and nothing the compiler might turn into a call (no
// memcpy-able loops: copies go through volatile pointers). Its stubs return
// the cartridge to bank 1 (BT_HOME). Its cart stubs sit just below the
// window ($9Exx-$9Fxx), where the screen was: btMoveScreen() lowers RAMTOP
// by 2 KB first. Only 2 KB: the C stack starts at the old MEMTOP, just below
// the old screen, and keeps the 1 KB between. Its probes are the first byte
// of cartridge banks 2 and 3. MegaCart's 16 KB bank 0 has room, so there
// everything stays in bank 0, its cart stubs sit beside the others below
// $4000, and the screen stays put. stress's own bankN_probe()s are only
// linked when testCartBank() is; calibration would catch a misplaced probe.

#define BT_ITERATIONS 200          // per test per round; X counts up to it

#ifdef STRESS_MEGACART
#define BT_CODE
#define BT_RODATA
#define BT_HOME 0                  // the cartridge bank this code runs in
#define BT_CART_LOOP    0x3f40
#define BT_CART_CAL     0x3f60
#define BT_CART_SCRATCH 0x3f80
#define BT_HAVE_BASIC 0
#define BT_MOVE_SCREEN 0           // the stubs sit below $4000, clear of it
#else
#define BT_CODE CART_BANK(1)
#define BT_RODATA __attribute__((section(".bank1.rodata")))
#define BT_HOME 1
#define BT_RAMTOP 0x98
#define BT_CART_LOOP    0x9f00
#define BT_CART_CAL     0x9f40
#define BT_CART_SCRATCH 0x9f80
#define BT_CART_SWITCH  0x9ffd
#define BT_HAVE_BASIC 1
#define BT_MOVE_SCREEN 1
#endif

// Extended memory: two RAMBO256/130XE banks (bit 4 low: CPU sees the bank).
#define BT_PORTB_A 0xe3
#define BT_PORTB_B 0xe7
#define BT_PORTB_MAIN 0xff
// Cartridge banks 2 and 3 hold the probes.
#define BT_CART_A 2
#define BT_CART_B 3
// PBI: deselected (A) and this board's device bit (B, PDEVNUM in asmDefs.h).
#define BT_PBI_A 0x00
#define BT_PBI_B 0x02
#define BT_PORTB_OS_OFF 0xfe
// BASIC: on (bit 1 low); off is BT_PORTB_MAIN (the RAM under it).
#define BT_PORTB_BASIC_ON 0xfd

#define BT_EXT_LOOP   0x3f00
#define BT_PBI_LOOP   0x3f20
#define BT_EXT_SWITCH 0x3ffd
#define BT_PBI_SWITCH 0xd7fd
#define BT_BASIC_LOOP  0x9e00
#define BT_BASIC_MISS_HANDLER 0x9e5d
#define BT_BASIC_PROBE 0x9e80
#define BT_BASIC_MISS  0x9f90      // 16-bit miss count, bumped by the handler
#define BT_BASIC_SAVEY 0x9f93
#define BT_BASIC_CALOFF 0x9f94     // $A000 with BASIC off: the seeded RTS
#define BT_BASIC_CALON 0x9f95      // $A000 with BASIC on: BASIC's opcode
#define BT_BASIC_OPCODE 0x9f96     // the startup probe's reading of it

// From bank 0: AtariMax through the bank trampoline, MegaCart directly.
#ifdef STRESS_MEGACART
#define BT_CALL(function) (function)()
#else
#define BT_CALL(function) CART_CALL(BT_HOME, (function))
#endif

#define BT_STR2(x) #x
#define BT_STR(x) BT_STR2(x)

#ifdef STRESS_MEGACART
// MegaCart: the toolchain copies bank 0's program to the start of every bank
// (so code survives a switch), and a bank's own sections follow it. So the
// switch itself sits in banks 2 and 3, at the same offset in each: the RAM
// loop selects bank 2 (A) and calls the preamble, LDA #3 / STA $D500; the
// fetch right after shows which bank is mapped: A returns, B counts in X.
// Two blocks: exact (the probe byte straight after the STA) and +1 (a NOP
// first: its fetch picks up the bank, and the probe comes a cycle later).
// btMcExact/btMcNop are bank 2's; bank 3 has the same layout. "R": nothing
// calls them by name.
__asm__(".section .bank2,\"axR\",@progbits\n"
        ".globl btMcExact\nbtMcExact:\n\tlda #3\n\tsta 0xd500\n\trts\n\tnop\n"
        ".globl btMcNop\nbtMcNop:\n\tlda #3\n\tsta 0xd500\n\tnop\n\trts\n\tnop\n"
        ".section .bank3,\"axR\",@progbits\n"
        "\tlda #3\n\tsta 0xd500\n\tinx\n\trts\n"
        "\tlda #3\n\tsta 0xd500\n\tnop\n\tinx\n\trts\n"
        ".text\n");
extern const uint8_t btMcExact[], btMcNop[];
#define BT_MC_EXACT_PROBE 5        // offset of the probe byte in each block
#define BT_MC_NOP_PROBE 6
#else
// AtariMax: the cartridge probes are the first byte of banks 2 (A: return
// at once) and 3 (B: count in X first). "R": nothing calls them by name.
__asm__(".section .bank2,\"axR\",@progbits\n"
        "cartProbeA:\n\trts\n"
        ".section .bank3,\"axR\",@progbits\n"
        "cartProbeB:\n\tinx\n\trts\n"
        ".text\n");
#endif

static volatile uint8_t * const btExtWin = (volatile uint8_t *)0x4000;
static volatile uint8_t * const btPbiWin = (volatile uint8_t *)0xd800;
static volatile uint8_t * const btRam = (volatile uint8_t *)0;

// $3F00, entered with X = 0 and Y = BT_ITERATIONS.
BT_RODATA static const uint8_t btExtLoop[] = {
    0xa9, BT_PORTB_A,       // 3F00  lda #PORTB_A
    0x8d, 0x01, 0xd3,       // 3F02  sta PORTB       select A
    0xa9, BT_PORTB_B,       // 3F05  lda #PORTB_B
    0x20, 0xfd, 0x3f,       // 3F07  jsr $3FFD       select B, fall into $4000
    0x88,                   // 3F0A  dey
    0xd0, 0xf3,             // 3F0B  bne $3F00
    0xa9, BT_PORTB_MAIN,    // 3F0D  lda #PORTB_MAIN
    0x8d, 0x01, 0xd3,       // 3F0F  sta PORTB
    0x60,                   // 3F12  rts
};
BT_RODATA static const uint8_t btExtSwitch[] = {
    0x8d, 0x01, 0xd3,       // 3FFD  sta PORTB       next fetch: $4000
};

// $3F20, entered with X = 0 and Y = BT_ITERATIONS, with the OS ROM off.
BT_RODATA static const uint8_t btPbiLoop[] = {
    0xa9, BT_PBI_A,         // 3F20  lda #PBI_A
    0x8d, 0xff, 0xd1,       // 3F22  sta $D1FF       deselect (A)
    0xa9, BT_PBI_B,         // 3F25  lda #PBI_B
    0x20, 0xfd, 0xd7,       // 3F27  jsr $D7FD       select (B), fall into $D800
    0x88,                   // 3F2A  dey
    0xd0, 0xf3,             // 3F2B  bne $3F20
    0xa9, BT_PBI_A,         // 3F2D  lda #PBI_A
    0x8d, 0xff, 0xd1,       // 3F2F  sta $D1FF
    0x60,                   // 3F32  rts
};
BT_RODATA static const uint8_t btPbiSwitch[] = {
    0x8d, 0xff, 0xd1,       // D7FD  sta $D1FF       next fetch: $D800
};

#ifdef STRESS_MEGACART
// $3F40, entered with X = 0 and Y = BT_ITERATIONS from cartridge bank 0.
// The JSR target (offsets 6-7) is patched in: the exact or the +1 block.
// JSR's stack pushes give the board bank A before the call lands.
BT_RODATA static const uint8_t btCartLoop[] = {
    0xa9, BT_CART_A,        // 3F40  lda #A
    0x8d, 0x00, 0xd5,       // 3F42  sta $D500       select A
    0x20, 0x00, 0x00,       // 3F45  jsr block       select B, fetch the probe
    0x88,                   // 3F48  dey
    0xd0, 0xf5,             // 3F49  bne $3F40
    0xa9, BT_HOME,          // 3F4B  lda #HOME
    0x8d, 0x00, 0xd5,       // 3F4D  sta $D500       back to bank 0
    0x8d, 0x80, 0x3f,       // 3F50  sta CART_SCRATCH
    0x60,                   // 3F53  rts
};
#define BT_CART_LOOP_JSR 6
// $3F60: select the bank in byte 1, settle with a write, and call the probe
// byte (patched into offsets 11-12).
BT_RODATA static const uint8_t btCartCal[] = {
    0xa9, 0x00,             // 3F60  lda #n          (n patched in)
    0x8d, 0x00, 0xd5,       // 3F62  sta $D500
    0x8d, 0x80, 0x3f,       // 3F65  sta CART_SCRATCH
    0xa2, 0x00,             // 3F68  ldx #0
    0x20, 0x00, 0x00,       // 3F6A  jsr probe
    0xa9, BT_HOME,          // 3F6D  lda #HOME
    0x8d, 0x00, 0xd5,       // 3F6F  sta $D500       back to bank 0
    0x8d, 0x80, 0x3f,       // 3F72  sta CART_SCRATCH
    0x60,                   // 3F75  rts
};
#define BT_CART_CAL_JSR 11
#else
// $9F00, entered with X = 0 and Y = BT_ITERATIONS from cartridge bank 1.
// JSR's stack pushes are writes, so the board has bank A by the switch. The
// write after the return to bank 1 lets the board take it before RTS.
BT_RODATA static const uint8_t btCartLoop[] = {
    0x8d, BT_CART_A, 0xd5,  // 9F00  sta $D500+A     select A
    0x20, 0xfd, 0x9f,       // 9F03  jsr $9FFD       select B, fall into $A000
    0x88,                   // 9F06  dey
    0xd0, 0xf7,             // 9F07  bne $9F00
    0x8d, BT_HOME, 0xd5,    // 9F09  sta $D500+HOME  back to bank 1
    0x8d, 0x80, 0x9f,       // 9F0C  sta CART_SCRATCH
    0x60,                   // 9F0F  rts
};
BT_RODATA static const uint8_t btCartSwitch[] = {
    0x8d, BT_CART_B, 0xd5,  // 9FFD  sta $D500+B     next fetch: $A000
};
// $9F40: select the bank in byte 1, settle with a write, and call $A000.
BT_RODATA static const uint8_t btCartCal[] = {
    0x8d, 0x00, 0xd5,       // 9F40  sta $D500+n     (n patched in)
    0x8d, 0x80, 0x9f,       // 9F43  sta CART_SCRATCH
    0xa2, 0x00,             // 9F46  ldx #0
    0x20, 0x00, 0xa0,       // 9F48  jsr $A000
    0x8d, BT_HOME, 0xd5,    // 9F4B  sta $D500+HOME  back to bank 1
    0x8d, 0x80, 0x9f,       // 9F4E  sta CART_SCRATCH
    0x60,                   // 9F51  rts
};

// $9E00, entered with Y = BT_ITERATIONS from cartridge bank 1. The four
// seed bytes ($A001-$A004) are patched in at offsets 0x14, 0x19, 0x1E, 0x23.
BT_RODATA static const uint8_t btBasicLoop[] = {
    0x8d, 0x10, 0xd5,       // 9E00  sta $D510       cartridge off
    0x8d, 0x80, 0x9f,       // 9E03  sta CART_SCRATCH
    0xa9, BT_PORTB_MAIN,    // 9E06  lda #$FF        BASIC off: RAM
    0x8d, 0x01, 0xd3,       // 9E08  sta PORTB
    0xad, 0x80, 0x9f,       // 9E0B  lda CART_SCRATCH    settle
    0xa9, 0x60,             // 9E0E  lda #$60
    0x8d, 0x00, 0xa0,       // 9E10  sta $A000       RTS: the correct path
    0xa9, 0x00,             // 9E13  lda #seed1
    0x8d, 0x01, 0xa0,       // 9E15  sta $A001
    0xa9, 0x00,             // 9E18  lda #seed2
    0x8d, 0x02, 0xa0,       // 9E1A  sta $A002
    0xa9, 0x00,             // 9E1D  lda #seed3
    0x8d, 0x03, 0xa0,       // 9E1F  sta $A003
    0xa9, 0x00,             // 9E22  lda #seed4
    0x8d, 0x04, 0xa0,       // 9E24  sta $A004
    0xad, 0x00, 0xa0,       // 9E27  lda $A000       calibrate: BASIC off
    0x8d, 0x94, 0x9f,       // 9E2A  sta BASIC_CALOFF
    0xa9, BT_PORTB_BASIC_ON,// 9E2D  lda #$FD
    0x8d, 0x01, 0xd3,       // 9E2F  sta PORTB       BASIC on
    0xad, 0x80, 0x9f,       // 9E32  lda CART_SCRATCH    settle
    0xad, 0x00, 0xa0,       // 9E35  lda $A000       calibrate: BASIC on
    0x8d, 0x95, 0x9f,       // 9E38  sta BASIC_CALON
    0xa9, BT_PORTB_BASIC_ON,// 9E3B  lda #$FD        loop:
    0x8d, 0x01, 0xd3,       // 9E3D  sta PORTB       BASIC on (A)
    0x8c, 0x93, 0x9f,       // 9E40  sty BASIC_SAVEY     (BASIC's opcode may be LDY)
    0xa9, BT_PORTB_MAIN,    // 9E43  lda #$FF
    0x20, 0xfd, 0x9f,       // 9E45  jsr $9FFD       BASIC off (B), fall into $A000
    0xac, 0x93, 0x9f,       // 9E48  ldy BASIC_SAVEY
    0x88,                   // 9E4B  dey
    0xd0, 0xed,             // 9E4C  bne loop ($9E3B)
    0xa9, BT_PORTB_MAIN,    // 9E4E  lda #$FF
    0x8d, 0x01, 0xd3,       // 9E50  sta PORTB
    0xad, 0x80, 0x9f,       // 9E53  lda CART_SCRATCH    settle
    0x8d, BT_HOME, 0xd5,    // 9E56  sta $D500+HOME  cartridge bank 1 back
    0x8d, 0x80, 0x9f,       // 9E59  sta CART_SCRATCH
    0x60,                   // 9E5C  rts
    0xee, 0x90, 0x9f,       // 9E5D  inc BASIC_MISS  basicMiss: a late fetch
    0xd0, 0x03,             // 9E60  bne $9E65
    0xee, 0x91, 0x9f,       // 9E62  inc BASIC_MISS+1
    0x60,                   // 9E65  rts             back through the loop's JSR
};
BT_RODATA static const uint8_t btBasicSwitch[] = {
    0x8d, 0x01, 0xd3,       // 9FFD  sta PORTB       next fetch: $A000
};
// $9E80: BASIC's opcode at $A000, read with the cartridge off.
BT_RODATA static const uint8_t btBasicProbe[] = {
    0x8d, 0x10, 0xd5,       // 9E80  sta $D510       cartridge off
    0x8d, 0x80, 0x9f,       // 9E83  sta CART_SCRATCH
    0xa9, BT_PORTB_BASIC_ON,// 9E86  lda #$FD
    0x8d, 0x01, 0xd3,       // 9E88  sta PORTB       BASIC on
    0xad, 0x80, 0x9f,       // 9E8B  lda CART_SCRATCH    settle
    0xad, 0x00, 0xa0,       // 9E8E  lda $A000
    0x8d, 0x96, 0x9f,       // 9E91  sta BASIC_OPCODE
    0xa9, BT_PORTB_MAIN,    // 9E94  lda #$FF
    0x8d, 0x01, 0xd3,       // 9E96  sta PORTB       BASIC off
    0xad, 0x80, 0x9f,       // 9E99  lda CART_SCRATCH    settle
    0x8d, BT_HOME, 0xd5,    // 9E9C  sta $D500+HOME  cartridge bank 1 back
    0x8d, 0x80, 0x9f,       // 9E9F  sta CART_SCRATCH
    0x60,                   // 9EA2  rts
};
// BASIC opcodes taking a 2-byte zero-page or immediate form.
BT_RODATA static const uint8_t btTwoByteOps[] = {
    0xa5, 0xa6, 0xa4, 0xa9, 0xa2, 0xa0,     // LDA/LDX/LDY zp, #
    0xc5, 0xc9, 0xe4, 0xe0, 0xc4, 0xc0,     // CMP/CPX/CPY zp, #
    0x24, 0x05, 0x09, 0x25, 0x29, 0x45, 0x49, // BIT zp, ORA/AND/EOR zp, #
};
#endif

// Totals since boot. A round whose calibration fails doesn't count, so the
// miss counts are of the rounds that calibrated. calA/calB count
// calibrations where bank A or bank B answered wrong. cartLate is MegaCart's
// exact fall-through, expected to miss on the board (one cycle late).
struct BtTally {
    uint32_t expected, counted, calA, calB;
};
struct BtTally btExt, btCart, btCartLate, btPbi, btBasic;
uint8_t btBasicOk, btBasicOpcode;

// A copy into RAM through a volatile pointer: no memcpy call (bank 0).
BT_CODE static void btCopy(uint16_t dest, const uint8_t *src, uint8_t len) {
    volatile uint8_t *d = (volatile uint8_t *)dest;
    for (uint8_t i = 0; i < len; ++i) d[i] = src[i];
}

BT_CODE static void btIrqOff(void) {
    __asm__ volatile("sei" ::: "memory");
    *nmien = 0;
}

BT_CODE static void btIrqOn(void) {
    *nmien = 0x40;          // VBI, the OS default
    __asm__ volatile("cli" ::: "memory");
}

// Add a round's hits; nothing is printed here.
BT_CODE static void btTally(struct BtTally *t, uint8_t badA, uint8_t badB, uint8_t hits) {
    if (badA) ++t->calA;
    if (badB) ++t->calB;
    if (badA || badB) return;
    t->expected += BT_ITERATIONS;
    t->counted += hits;
}

BT_CODE static void btSeedExt(void) {
    // Repeated writes and a read give the board time to switch.
    *portb = BT_PORTB_A; *portb = BT_PORTB_A; (void)*btExtWin;
    btExtWin[0] = 0x60;                       // rts
    *portb = BT_PORTB_B; *portb = BT_PORTB_B; (void)*btExtWin;
    btExtWin[0] = 0xe8;                       // inx
    btExtWin[1] = 0x60;                       // rts
    *portb = BT_PORTB_MAIN;
}

// X after a settled select of the bank and a JSR to $4000.
BT_CODE static uint8_t btCalExt(uint8_t portbValue) {
    uint8_t x;
    *portb = portbValue; *portb = portbValue; (void)*btExtWin;
    __asm__ volatile("ldx #0\n\tjsr 0x4000" : "=x"(x) : : "a", "y", "p", "memory");
    *portb = BT_PORTB_MAIN;
    return x;
}

BT_CODE static void btTestExt(void) {
    btIrqOff();
    btSeedExt();
    uint8_t calA = btCalExt(BT_PORTB_A), calB = btCalExt(BT_PORTB_B), x = 0, y = BT_ITERATIONS;
    if (calA == 0 && calB == 1)
        __asm__ volatile("ldx #0\n\tjsr " BT_STR(BT_EXT_LOOP) : "=x"(x), "+y"(y) : : "a", "p", "memory");
    btIrqOn();
    btTally(&btExt, calA != 0, calB != 1, x);
}

// With the OS ROM off: $D800 in RAM (deselected) gets RTS, and the PBI
// ROM's checksum bytes (selected) get INX, RTS until btRestorePbi().
BT_CODE static void btSelectPbi(uint8_t mask) {
    *newport = mask; *newport = mask; (void)*btPbiWin;
}

BT_CODE static uint8_t btCalPbi(uint8_t mask) {
    uint8_t x;
    btSelectPbi(mask);
    __asm__ volatile("ldx #0\n\tjsr 0xd800" : "=x"(x) : : "a", "y", "p", "memory");
    btSelectPbi(BT_PBI_A);
    return x;
}

// Nothing here may touch the OS ROM: it is off from the first PORTB write
// to the last, with IRQs and NMIs masked.
BT_CODE static void btTestPbi(void) {
    btIrqOff();
    *portb = BT_PORTB_OS_OFF; *portb = BT_PORTB_OS_OFF; (void)*btExtWin;
    btSelectPbi(BT_PBI_A);
    btPbiWin[0] = 0x60;                       // rts
    btSelectPbi(BT_PBI_B);
    btPbiWin[0] = 0xe8;                       // inx
    btPbiWin[1] = 0x60;                       // rts
    btSelectPbi(BT_PBI_A);
    uint8_t calA = btCalPbi(BT_PBI_A), calB = btCalPbi(BT_PBI_B), x = 0, y = BT_ITERATIONS;
    if (calA == 0 && calB == 1)
        __asm__ volatile("ldx #0\n\tjsr " BT_STR(BT_PBI_LOOP) : "=x"(x), "+y"(y) : : "a", "p", "memory");
    btSelectPbi(BT_PBI_B);                    // restore the checksum bytes,
    btPbiWin[0] = 0xff;                       // as in main/pbirom.asm
    btPbiWin[1] = 0xff;
    btSelectPbi(BT_PBI_A);
    *portb = BT_PORTB_MAIN; *portb = BT_PORTB_MAIN; (void)*btExtWin;
    btIrqOn();
    btTally(&btPbi, calA != 0, calB != 1, x);
}

BT_CODE static uint8_t btCalCart(uint8_t bank) {
    uint8_t x;
    btRam[BT_CART_CAL + 1] = bank;
    __asm__ volatile("jsr " BT_STR(BT_CART_CAL) : "=x"(x) : : "a", "y", "p", "memory");
    return x;
}

BT_CODE static void btTestCart(struct BtTally *t) {
    btIrqOff();
    uint8_t calA = btCalCart(BT_CART_A), calB = btCalCart(BT_CART_B), x = 0, y = BT_ITERATIONS;
    if (calA == 0 && calB == 1)
        __asm__ volatile("ldx #0\n\tjsr " BT_STR(BT_CART_LOOP) : "=x"(x), "+y"(y) : : "a", "p", "memory");
    btIrqOn();
    btTally(t, calA != 0, calB != 1, x);
}

#if BT_HAVE_BASIC
// Seeds for $A001-$A004 by BASIC's first opcode, patched into the RAM copy
// of the loop. A 2-byte zero-page or immediate op takes $A001 as its operand
// ($D4, an FP register: a harmless read), then runs JMP basicMiss from
// $A002. JMP abs jumps straight there. Returns 0 for any other opcode.
BT_CODE static uint8_t btBasicSeeds(uint8_t opcode) {
    uint8_t s1, s2, s3, s4, twoByte = 0;
    for (uint8_t i = 0; i < sizeof(btTwoByteOps); ++i)
        if (btTwoByteOps[i] == opcode) twoByte = 1;
    if (opcode == 0x4c) {
        s1 = BT_BASIC_MISS_HANDLER & 0xff; s2 = BT_BASIC_MISS_HANDLER >> 8; s3 = s4 = 0xea;
    } else if (twoByte) {
        s1 = 0xd4; s2 = 0x4c; s3 = BT_BASIC_MISS_HANDLER & 0xff; s4 = BT_BASIC_MISS_HANDLER >> 8;
    } else {
        return 0;
    }
    btRam[BT_BASIC_LOOP + 0x14] = s1; btRam[BT_BASIC_LOOP + 0x19] = s2;
    btRam[BT_BASIC_LOOP + 0x1e] = s3; btRam[BT_BASIC_LOOP + 0x23] = s4;
    return 1;
}

// Calibration and the loop run inside BT_BASIC_LOOP; hits are the
// iterations that didn't reach basicMiss. It borrows the cart switch slot.
BT_CODE static void btTestBasic(void) {
    uint8_t y = BT_ITERATIONS;
    btCopy(BT_CART_SWITCH, btBasicSwitch, sizeof(btBasicSwitch));
    btRam[BT_BASIC_MISS] = btRam[BT_BASIC_MISS + 1] = 0;
    btIrqOff();
    __asm__ volatile("jsr " BT_STR(BT_BASIC_LOOP) : "+y"(y) : : "a", "x", "p", "memory");
    btIrqOn();
    btCopy(BT_CART_SWITCH, btCartSwitch, sizeof(btCartSwitch));
    // Plain ifs: as a ?: select this compiled to "bit __set_v", whose
    // byte is in bank 0's libc (bank 1 would test whatever sits there).
    uint8_t hits = 0, badA = 0, badB = 0;
    if (btRam[BT_BASIC_MISS + 1] == 0 && btRam[BT_BASIC_MISS] <= BT_ITERATIONS)
        hits = BT_ITERATIONS - btRam[BT_BASIC_MISS];
    if (btRam[BT_BASIC_CALOFF] != 0x60) badA = 1;
    if (btRam[BT_BASIC_CALON] != btRam[BT_BASIC_OPCODE]) badB = 1;
    btTally(&btBasic, badA, badB, hits);
}
#endif

#ifdef STRESS_MEGACART
// Point the RAM loop at a switch block and the calibration at its probe.
BT_CODE static void btPatchMc(const uint8_t *block, uint8_t probe) {
    const uint16_t entry = (uint16_t)block, probeAddr = entry + probe;
    btRam[BT_CART_LOOP + BT_CART_LOOP_JSR] = entry & 0xff;
    btRam[BT_CART_LOOP + BT_CART_LOOP_JSR + 1] = entry >> 8;
    btRam[BT_CART_CAL + BT_CART_CAL_JSR] = probeAddr & 0xff;
    btRam[BT_CART_CAL + BT_CART_CAL_JSR + 1] = probeAddr >> 8;
}
#endif

BT_CODE static void btInstall(void) {
    btCopy(BT_EXT_LOOP, btExtLoop, sizeof(btExtLoop));
    btCopy(BT_EXT_SWITCH, btExtSwitch, sizeof(btExtSwitch));
    btCopy(BT_PBI_LOOP, btPbiLoop, sizeof(btPbiLoop));
    btCopy(BT_PBI_SWITCH, btPbiSwitch, sizeof(btPbiSwitch));
    btCopy(BT_CART_LOOP, btCartLoop, sizeof(btCartLoop));
    btCopy(BT_CART_CAL, btCartCal, sizeof(btCartCal));
#ifndef STRESS_MEGACART
    btCopy(BT_CART_SWITCH, btCartSwitch, sizeof(btCartSwitch));
    btCopy(BT_BASIC_LOOP, btBasicLoop, sizeof(btBasicLoop));
#endif
}

#if BT_MOVE_SCREEN
// CIO through the OS's raw IOCB 0 ($0340): RAMTOP ($6A) down 2 KB, and E:
// reopened below it, so the screen leaves the stubs' area (see Layout).
// The device name is in this bank, since CIO reads it while it's mapped.
BT_RODATA static const char btEDevice[] = "E:";
BT_CODE static void btCio(void) {
    __asm__ volatile("ldx #0\n\tjsr 0xe456" : : : "a", "x", "y", "p", "memory");
}

BT_CODE static void btMoveScreen(void) {
    volatile uint8_t *iocb0 = (volatile uint8_t *)0x340;
    btRam[0x6a] = BT_RAMTOP;
    iocb0[2] = 0x0c;                          // CLOSE
    btCio();
    iocb0[2] = 0x03;                          // OPEN
    iocb0[4] = (uint8_t)((uint16_t)btEDevice & 0xff);
    iocb0[5] = (uint8_t)((uint16_t)btEDevice >> 8);
    iocb0[10] = 12;                           // aux1: read/write
    iocb0[11] = 0;
    btCio();
}
#endif

// The report line, built here (bank 1 on AtariMax: no printf) into RAM for
// bank 0 to print. Decimal by subtracting powers of ten: a division would
// call the compiler runtime in bank 0.
char btLine[96];
static uint8_t btLineLen;
BT_RODATA static const uint32_t btPow10[] = {
    1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1,
};
BT_RODATA static const char btHex[] = "0123456789abcdef";
BT_RODATA static const char btLblBank[] = " bank ext:";
BT_RODATA static const char btLblCart[] = " cart:";
BT_RODATA static const char btLblPbi[] = " pbi:";
BT_RODATA static const char btLblBasic[] = " basic:";
BT_RODATA static const char btLblLate[] = " late:";
BT_RODATA static const char btLblCal[] = " cal:";
BT_RODATA static const char btLblSetup[] = "bank tests: basic $A000=$";
BT_RODATA static const char btLblSkipped[] = ", skipped";

BT_CODE static void btPutC(char c) {
    if (btLineLen < sizeof(btLine) - 1) btLine[btLineLen++] = c;
    btLine[btLineLen] = 0;
}
BT_CODE static void btPutStr(const char *text) {
    while (*text) btPutC(*text++);
}
BT_CODE static void btPutDec(uint32_t value) {
    uint8_t started = 0;
    for (uint8_t i = 0; i < sizeof(btPow10) / sizeof(btPow10[0]); ++i) {
        char digit = '0';
        while (value >= btPow10[i]) { value -= btPow10[i]; ++digit; }
        if (digit != '0' || started || i == sizeof(btPow10) / sizeof(btPow10[0]) - 1) {
            btPutC(digit);
            started = 1;
        }
    }
}
// " label misses/tries"
BT_CODE static void btPutTally(const char *label, const struct BtTally *t) {
    btPutStr(label);
    btPutDec(t->expected - t->counted);
    btPutC('/');
    btPutDec(t->expected);
}

// Next-cycle bank switches: misses/tries per test, then calibration
// failures (all tests). MegaCart's "late" is the exact fall-through,
// expected to miss on the board; its "cart" has the NOP.
BT_CODE void btReport(void) {
    btLineLen = 0;
    btPutTally(btLblBank, &btExt);
    btPutTally(btLblCart, &btCart);
    btPutTally(btLblPbi, &btPbi);
#if BT_HAVE_BASIC
    btPutTally(btLblBasic, &btBasic);
#endif
#ifdef STRESS_MEGACART
    btPutTally(btLblLate, &btCartLate);
#endif
    btPutStr(btLblCal);
    btPutDec(btExt.calA + btExt.calB + btCart.calA + btCart.calB + btCartLate.calA + btCartLate.calB +
             btPbi.calA + btPbi.calB + btBasic.calA + btBasic.calB);
    btPutC('\n');
}

// Once, before the first round (called from bank 0 via CART_CALL).
BT_CODE void btSetup(void) {
#if BT_MOVE_SCREEN
    btMoveScreen();
#endif
    btInstall();
#if BT_HAVE_BASIC
    btCopy(BT_BASIC_PROBE, btBasicProbe, sizeof(btBasicProbe));
    btIrqOff();
    __asm__ volatile("jsr " BT_STR(BT_BASIC_PROBE) : : : "a", "x", "y", "p", "memory");
    btIrqOn();
    btBasicOpcode = btRam[BT_BASIC_OPCODE];
    btBasicOk = btBasicSeeds(btBasicOpcode);
#endif
    btLineLen = 0;
#if BT_HAVE_BASIC
    btPutStr(btLblSetup);
    btPutC(btHex[btBasicOpcode >> 4]);
    btPutC(btHex[btBasicOpcode & 15]);
    if (!btBasicOk) btPutStr(btLblSkipped);
    btPutC('\n');
#endif
}

// One round of every test that applies (called from bank 0 via CART_CALL).
// The ext test needs extended memory (probeExtMem() found a layout), and is
// left out of the MegaCart build: its C stack starts at MEMTOP (~$7C00),
// inside the $4000-$7FFF bank window, so C code that touched it with an
// ext bank selected would corrupt it. The AtariMax build covers ext.
BT_CODE void btRound(void) {
    btInstall();
#if BT_HAVE_BASIC
    btBasicOk = btBasicSeeds(btBasicOpcode);
#endif
#ifndef STRESS_MEGACART
    if (bankType >= 0) btTestExt();
#endif
#ifdef STRESS_MEGACART
    btPatchMc(btMcNop, BT_MC_NOP_PROBE);
    btTestCart(&btCart);                      // +1 cycle
    btPatchMc(btMcExact, BT_MC_EXACT_PROBE);
    btTestCart(&btCartLate);                  // exact: misses on the board
#else
    btTestCart(&btCart);
#endif
    btTestPbi();
#if BT_HAVE_BASIC
    if (btBasicOk) btTestBasic();
#endif
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
    BT_CALL(btSetup);
    printf("%s", btLine);
    int f = open("J1:DUMPSCREEN", O_CREAT);
    close(f);

    long loopCount = 0;
    //testExtMem(1, 1);
    while(1) { 
        //CART_CALL(1, testCartBankBanked);
        testCio(1);
        //testDos(1);
        //CART_CALL(1, testOsBanked);
        testExtMem(1, 0);
        BT_CALL(btRound);
        printf("%ld: cio:%ld cart:%ld os:%ld ext:%ld dos:%ld\n",
                loopCount, cioErrors, cartBankErrors, osErrors, memErrors, dosErrors);
        BT_CALL(btReport);
        printf("%s", btLine);
        loopCount++;
    }
}
