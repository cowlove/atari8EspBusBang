// banktest: does a bank switch reach the very next bus cycle?
//
// On the Atari, a PORTB write switches the extended-memory bank, a $D5xx
// access the cartridge bank, and a $D1FF write the PBI ROM at $D800, in time
// for the very next cycle. The board applies PORTB and $D1FF late, at the end
// of the next read cycle; the AtariMax $D5xx select lands in the write cycle
// itself. This test checks the hardware behavior, so it passes on a real
// Atari (and on atari800's own banking) and counts misses on the board until
// core 1 applies the switches on time.
//
// The only read that directly follows a write is the next opcode fetch, so
// each test switches with an STA in the last three bytes before the banked
// window and falls through into it:
//
//   ext:  $3FFD  STA $D301    falls into $4000
//   cart: $9FFD  STA $D500+B  falls into $A000
//   pbi:  $D7FD  STA $D1FF    falls into $D800
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
// banks apart: by the next read the board has caught up, even for the
// cartridge, because both banks then agree.
//
// The loops run from RAM (the cartridge test switches the program's own
// bank out) with IRQs and NMIs off. ANTIC DMA stays on: a DMA read between
// the STA and the fetch can let the board catch up by luck, so a miss is
// proof of the late switch, but a clean count from the board alone is not
// proof of a fix. Run with DMA off (SDMCTL) for that.
//
// Before each test, a calibration call selects each bank with time to
// settle and JSRs into the window, to check that the seeding took.
//
// The BANKTEST boot config gives the board one SRAM bank for the two, so
// every PORTB switch evicts: the board halts the 6502 on the STA while it
// pages the bank in from PSRAM, and the fetch after the halt must still see
// the new bank. Stressing that transition is part of the point.

#include <atari.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define ITERATIONS 200          // per round; X counts up to it
#define REPORT_JIFFIES 300      // ~5 s between reports (NTSC; 6 s PAL)

// Extended memory: two RAMBO256/130XE banks (bit 4 low: CPU sees the bank).
#define PORTB_A 0xe3
#define PORTB_B 0xe7
#define PORTB_MAIN 0xff
// Cartridge: AtariMax 128 banks (an access to $D500+n selects bank n). The
// program runs in bank 0; banktest.s puts the probes at $A000 of banks 2, 3.
#define CART_A 2
#define CART_B 3
// PBI: deselected (A) and this board's device bit (B, PDEVNUM in asmDefs.h).
#define PBI_A 0x00
#define PBI_B 0x02
#define PORTB_OS_OFF 0xfe

static volatile uint8_t * const portb  = (volatile uint8_t *)0xd301;
static volatile uint8_t * const nmien  = (volatile uint8_t *)0xd40e;
static volatile uint8_t * const extWin = (volatile uint8_t *)0x4000;
static volatile uint8_t * const d1ff   = (volatile uint8_t *)0xd1ff;
static volatile uint8_t * const pbiWin = (volatile uint8_t *)0xd800;

// RAM code addresses. $9Cxx-$9Fxx is free once main() lowers RAMTOP and
// reopens the screen below it; $3F00-$4001 is above the program's RAM.
#define EXT_LOOP    0x3f00
#define PBI_LOOP    0x3f20
#define EXT_SWITCH  0x3ffd
#define PBI_SWITCH  0xd7fd
#define CART_LOOP   0x9f00
#define CART_CAL    0x9f40
#define CART_SCRATCH 0x9f80
#define CART_SWITCH 0x9ffd

// $3F00, entered with X = 0 and Y = ITERATIONS.
static const uint8_t extLoop[] = {
    0xa9, PORTB_A,          // 3F00  lda #PORTB_A
    0x8d, 0x01, 0xd3,       // 3F02  sta PORTB       select A
    0xa9, PORTB_B,          // 3F05  lda #PORTB_B
    0x20, 0xfd, 0x3f,       // 3F07  jsr $3FFD       select B, fall into $4000
    0x88,                   // 3F0A  dey
    0xd0, 0xf3,             // 3F0B  bne $3F00
    0xa9, PORTB_MAIN,       // 3F0D  lda #PORTB_MAIN
    0x8d, 0x01, 0xd3,       // 3F0F  sta PORTB
    0x60,                   // 3F12  rts
};
static const uint8_t extSwitch[] = {
    0x8d, 0x01, 0xd3,       // 3FFD  sta PORTB       next fetch: $4000
};

// $3F20, entered with X = 0 and Y = ITERATIONS, with the OS ROM off.
static const uint8_t pbiLoop[] = {
    0xa9, PBI_A,            // 3F20  lda #PBI_A
    0x8d, 0xff, 0xd1,       // 3F22  sta $D1FF       deselect (A)
    0xa9, PBI_B,            // 3F25  lda #PBI_B
    0x20, 0xfd, 0xd7,       // 3F27  jsr $D7FD       select (B), fall into $D800
    0x88,                   // 3F2A  dey
    0xd0, 0xf3,             // 3F2B  bne $3F20
    0xa9, PBI_A,            // 3F2D  lda #PBI_A
    0x8d, 0xff, 0xd1,       // 3F2F  sta $D1FF
    0x60,                   // 3F32  rts
};
static const uint8_t pbiSwitch[] = {
    0x8d, 0xff, 0xd1,       // D7FD  sta $D1FF       next fetch: $D800
};

// $9F00, entered with X = 0 and Y = ITERATIONS, called from cartridge bank 0.
// JSR's stack pushes are writes, so the board has bank A by the switch. The
// write after the return to bank 0 lets the board take bank 0 before RTS
// fetches from it.
static const uint8_t cartLoop[] = {
    0x8d, CART_A, 0xd5,     // 9F00  sta $D500+A     select A
    0x20, 0xfd, 0x9f,       // 9F03  jsr $9FFD       select B, fall into $A000
    0x88,                   // 9F06  dey
    0xd0, 0xf7,             // 9F07  bne $9F00
    0x8d, 0x00, 0xd5,       // 9F09  sta $D500       back to bank 0
    0x8d, 0x80, 0x9f,       // 9F0C  sta CART_SCRATCH
    0x60,                   // 9F0F  rts
};
static const uint8_t cartSwitch[] = {
    0x8d, CART_B, 0xd5,     // 9FFD  sta $D500+B     next fetch: $A000
};
// $9F40: select the bank in byte 1, settle with a write, and call $A000.
static const uint8_t cartCal[] = {
    0x8d, 0x00, 0xd5,       // 9F40  sta $D500+n     (n patched in)
    0x8d, 0x80, 0x9f,       // 9F43  sta CART_SCRATCH
    0xa2, 0x00,             // 9F46  ldx #0
    0x20, 0x00, 0xa0,       // 9F48  jsr $A000
    0x8d, 0x00, 0xd5,       // 9F4B  sta $D500       back to bank 0
    0x8d, 0x80, 0x9f,       // 9F4E  sta CART_SCRATCH
    0x60,                   // 9F51  rts
};

static void irqOff(void) {
    __asm__ volatile("sei" ::: "memory");
    *nmien = 0;
}

static void irqOn(void) {
    *nmien = 0x40;          // VBI, the OS default
    __asm__ volatile("cli" ::: "memory");
}

// JSR to a RAM loop with X = 0 and Y = count; returns X.
static uint8_t runLoop(uint16_t addr, uint8_t count) {
    uint8_t x;
    if (addr == EXT_LOOP)
        __asm__ volatile("ldx #0\n\tjsr $3f00" : "=x"(x), "+y"(count) : : "a", "p", "memory");
    else if (addr == PBI_LOOP)
        __asm__ volatile("ldx #0\n\tjsr $3f20" : "=x"(x), "+y"(count) : : "a", "p", "memory");
    else
        __asm__ volatile("ldx #0\n\tjsr $9f00" : "=x"(x), "+y"(count) : : "a", "p", "memory");
    return x;
}

static void seedExt(void) {
    // Repeated writes and a read give the board time to switch.
    *portb = PORTB_A; *portb = PORTB_A; (void)*extWin;
    extWin[0] = 0x60;                       // rts
    *portb = PORTB_B; *portb = PORTB_B; (void)*extWin;
    extWin[0] = 0xe8;                       // inx
    extWin[1] = 0x60;                       // rts
    *portb = PORTB_MAIN;
}

// X after a settled select of the bank and a JSR to $4000.
static uint8_t calExt(uint8_t portbValue) {
    uint8_t x;
    *portb = portbValue; *portb = portbValue; (void)*extWin;
    __asm__ volatile("ldx #0\n\tjsr $4000" : "=x"(x) : : "a", "y", "p", "memory");
    *portb = PORTB_MAIN;
    return x;
}

// With the OS ROM off: $D800 in RAM (deselected) gets RTS, and the PBI
// ROM's checksum bytes (selected) get INX, RTS until restorePbi().
static void selectPbi(uint8_t mask) {
    *d1ff = mask; *d1ff = mask; (void)*pbiWin;
}

static void seedPbi(void) {
    selectPbi(PBI_A);
    pbiWin[0] = 0x60;                       // rts
    selectPbi(PBI_B);
    pbiWin[0] = 0xe8;                       // inx
    pbiWin[1] = 0x60;                       // rts
    selectPbi(PBI_A);
}

static void restorePbi(void) {
    selectPbi(PBI_B);
    pbiWin[0] = 0xff;                       // as in main/pbirom.asm
    pbiWin[1] = 0xff;
    selectPbi(PBI_A);
}

static uint8_t calPbi(uint8_t mask) {
    uint8_t x;
    selectPbi(mask);
    __asm__ volatile("ldx #0\n\tjsr $d800" : "=x"(x) : : "a", "y", "p", "memory");
    selectPbi(PBI_A);
    return x;
}

static uint8_t calCart(uint8_t bank) {
    uint8_t x;
    ((volatile uint8_t *)CART_CAL)[1] = bank;
    __asm__ volatile("jsr $9f40" : "=x"(x) : : "a", "y", "p", "memory");
    return x;
}

static void installStubs(void) {
    memcpy((void *)EXT_LOOP, extLoop, sizeof(extLoop));
    memcpy((void *)EXT_SWITCH, extSwitch, sizeof(extSwitch));
    memcpy((void *)PBI_LOOP, pbiLoop, sizeof(pbiLoop));
    memcpy((void *)PBI_SWITCH, pbiSwitch, sizeof(pbiSwitch));
    memcpy((void *)CART_LOOP, cartLoop, sizeof(cartLoop));
    memcpy((void *)CART_CAL, cartCal, sizeof(cartCal));
    memcpy((void *)CART_SWITCH, cartSwitch, sizeof(cartSwitch));
}

static unsigned char cio(unsigned char channel) {
    unsigned char iocbOffset = (unsigned char)(channel << 4);
    __asm__ volatile("tax\n\tjsr $E456" : "+a"(iocbOffset) : : "x", "y", "memory");
    return OS.iocb[channel].status;
}

// Lower RAMTOP by 1 KB and reopen E:, so the screen and display list move
// below $9C00 and leave $9C00-$9FFF (the cartridge stubs) alone.
static void moveScreen(void) {
    OS.ramtop = 0x9c;
    OS.iocb[0].command = IOCB_CLOSE;
    cio(0);
    OS.iocb[0].command = IOCB_OPEN;
    OS.iocb[0].buffer = "E:";
    OS.iocb[0].aux1 = 12;
    OS.iocb[0].aux2 = 0;
    cio(0);
}

// A PBI CIO call: the board logs new screen lines ("ATARI: ...") on these.
static void pokeBoard(void) {
    int fd = open("J1:WDRESET", O_CREAT | O_WRONLY);
    if (fd >= 0) close(fd);
}

// Totals since boot. A round whose calibration fails skips its loop, so
// the miss counts are of the rounds that calibrated. calA/calB count
// calibrations where bank A or bank B answered wrong.
struct Tally {
    uint32_t expected, counted, calA, calB;
};
static struct Tally ext, cart, pbi;
static uint32_t rounds;

// Calibrate, and run the loop if that took; nothing is printed here.
static void tally(struct Tally *t, uint8_t calA, uint8_t calB, uint8_t x) {
    if (calA != 0) ++t->calA;
    if (calB != 1) ++t->calB;
    if (calA != 0 || calB != 1) return;
    t->expected += ITERATIONS;
    t->counted += x;
}

static void testExt(void) {
    irqOff();
    seedExt();
    uint8_t calA = calExt(PORTB_A), calB = calExt(PORTB_B);
    uint8_t x = calA == 0 && calB == 1 ? runLoop(EXT_LOOP, ITERATIONS) : 0;
    irqOn();
    tally(&ext, calA, calB, x);
}

static void testCart(void) {
    irqOff();
    uint8_t calA = calCart(CART_A), calB = calCart(CART_B);
    uint8_t x = calA == 0 && calB == 1 ? runLoop(CART_LOOP, ITERATIONS) : 0;
    irqOn();
    tally(&cart, calA, calB, x);
}

// Nothing here may touch the OS ROM: it is off from the first PORTB write
// to the last, with IRQs and NMIs masked.
static void testPbi(void) {
    irqOff();
    *portb = PORTB_OS_OFF; *portb = PORTB_OS_OFF; (void)*extWin;
    seedPbi();
    uint8_t calA = calPbi(PBI_A), calB = calPbi(PBI_B);
    uint8_t x = calA == 0 && calB == 1 ? runLoop(PBI_LOOP, ITERATIONS) : 0;
    restorePbi();
    *portb = PORTB_MAIN; *portb = PORTB_MAIN; (void)*extWin;
    irqOn();
    tally(&pbi, calA, calB, x);
}

int main(void) {
    moveScreen();
    installStubs();
    printf("\nBANKTEST: next-cycle bank switches\n");
    printf("ext PORTB $%02X->$%02X, cart bank %u->%u\n", PORTB_A, PORTB_B, CART_A, CART_B);
    printf("pbi $D1FF $%02X->$%02X\n", PBI_A, PBI_B);
    pokeBoard();

    // Rounds run flat out; a report every REPORT_JIFFIES. RTCLOK keeps
    // counting because the VBI runs between rounds. Each line fits 38
    // columns: misses/iterations, then the bad calibrations by bank.
    uint8_t lastJiffy = OS.rtclok[2];
    uint16_t jiffies = 0;
    for (uint32_t report = 1;; ++report) {
        while (jiffies < REPORT_JIFFIES) {
            installStubs();         // the screen editor can't reach them, but cheap
            testExt();
            testCart();
            testPbi();
            ++rounds;
            uint8_t now = OS.rtclok[2];
            jiffies += (uint8_t)(now - lastJiffy);
            lastJiffy = now;
        }
        jiffies -= REPORT_JIFFIES;
        int pass = ext.counted == ext.expected && cart.counted == cart.expected &&
                   pbi.counted == pbi.expected &&
                   ext.calA + ext.calB + cart.calA + cart.calB + pbi.calA + pbi.calB == 0;
        printf("%lu: %s %lu rounds\n", report, pass ? "PASS" : "FAIL", rounds);
        printf(" ext  %lu/%lu cal %lu %lu\n", ext.expected - ext.counted, ext.expected,
               ext.calA, ext.calB);
        printf(" cart %lu/%lu cal %lu %lu\n", cart.expected - cart.counted, cart.expected,
               cart.calA, cart.calB);
        printf(" pbi  %lu/%lu cal %lu %lu\n", pbi.expected - pbi.counted, pbi.expected,
               pbi.calA, pbi.calB);
        pokeBoard();
    }
}
