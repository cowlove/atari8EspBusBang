#ifndef A8BB_CSIM_ATARI800_H
#define A8BB_CSIM_ATARI800_H
/* CSIM: the line between atari800 (csim/a800/atari800, the 6502 and the
   Atari's chips) and the board (the host app, main/), the only declarations
   the two sides share. atari800's pbi.c and cpu.c call these; they are
   defined in csim/a800/csim_atari800.cpp. Plain C (atari800 is built
   -ansi -pedantic).

   Two modes, one build each:

   Default (embedded-call) mode: atari800 owns all of memory. It maps the
   board's PBI ROM (pbiROM[]) at $D800 itself on a $D1FF select, hands each
   PBI request to the board when the 6502 writes $D830/$D840, and calls
   the board's tick between instructions, which runs a pass of core 0's
   work in line. The board's MMU is never involved, which makes this mode
   a reference for it, and the fast one.

   Bus-cycle mode (make BUS_CYCLE=1, CSIM_BUS_CYCLE): every 6502 access is
   one bus cycle through core 1's loop body. If the board takes it (extSel_)
   it answers from its own map; otherwise atari800 does. PBI requests reach
   the board through its bus monitor, as on the hardware, and core 0 runs
   its real loop on a coroutine that each tick resumes.

   The board's side in both: it comes up on the first call that needs it
   (csim_board_setup()), and its PBI interrupt is the level
   csim_board_irq() reports, which atari800 ORs into the 6502's IRQ. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the board up over atari800's 64 KB (MEMORY_mem): the app's setup().
   Called once, at the first PBI request (default mode) or the first bus
   cycle (bus-cycle mode); later calls do nothing. */
void csim_board_setup(uint8_t *ram, size_t ramSize);

/* The 6502 ran `cycles` machine cycles (every 16 or more, and each halted
   cycle): advance the board's clock and run core 0. */
void csim_board_tick(uint32_t cycles);

/* The board's PBI interrupt line: nonzero while it requests an IRQ. */
int csim_board_irq(void);

/* atari800 -> the board: its machine clock in Hz (PAL or NTSC, as
   configured), which the board's tick converts to CCOUNT's 240 MHz. */
unsigned long csim_atari800_clock_hz(void);

/* The board's PBI ROM, the 2 KB atari800 maps at $D800-$DFFF while the
   board is selected (default mode: atari800 copies it in and out around
   each request; bus-cycle mode: the board serves it). */
extern uint8_t pbiROM[0x800];

/* Default mode: the 6502 wrote nonzero to $D830 or $D840 with the board's
   ROM mapped; `iocb` is that request's control block in pbiROM[]. Returns
   with the request handled. */
void csim_board_pbi_request(uint16_t addr, uint8_t *iocb);

/* Bus-cycle mode: one 6502 access through core 1's loop body. Nonzero if
   the board took it (asserted extSel_); then a read's byte is in *data. */
int csim_board_bus_cycle(uint16_t addr, int read, uint8_t *data);
/* Bus-cycle mode: the board is holding halt_. atari800 runs no
   instruction, but one csim_board_halt_cycle() and a one-cycle tick. */
int csim_board_halted(void);
void csim_board_halt_cycle(void);

/* The Atari's RESET line, as the Sidecar drives it (csim_atari_reset.cpp).
   Held: bus-cycle mode's 6502 runs no instruction, as for halt_ (the
   default mode doesn't stall). Released (or pulsed): the reset to run at
   atari800's next frame, consumed by the call: 0 none, 1 warm start
   (AKEY_WARMSTART), 2 cold (AKEY_COLDSTART). */
int csim_board_reset_held(void);
int csim_board_reset_request(void);

#ifdef __cplusplus
}
#endif

#endif
