#pragma once
// FAKE_SIO: a bench stress test of the PBI SIO path with no Atari. Core 0
// plays the 6502's side of the PBI ROM's SIO handler (main/pbirom.asm):
// it fills in the DCB and the request block, waits for the request to be
// served (req cleared), and does what the ROM would with the result:
// re-issue for REQ_FLAG_DETACHSAFE, copy in from "native" memory and
// re-issue for RES_FLAG_NEED_COPYIN, copy out for RES_FLAG_COPYOUT. The
// requests are disk writes of deterministic patterns and reads that verify
// them, to buffers all over the 6502's address space, so the SIO dispatch,
// the disk images (local and the Sidecar's), the mmu's mapped-or-copied
// decision and the copy in/out protocol are all checked end to end.
//
// The 6502's memory as the fake sees it: a mapped page (mmuState, as the bus
// would serve it) is ESP memory; anything else is native Atari RAM, which
// on the bench board is a shadow array (PSRAM) and in csim atari800's RAM.
//
// Built with -DFAKE_SIO: on hardware with FAKE_CLOCK (no 6502), in csim
// with a 6502 that stays off the PBI device (an idle cartridge, no PBI
// interrupts: BOOT_CONFIG=FAKESIO_CSIM).

#ifdef FAKE_SIO
// One step, from core0_utilityTasks(), before its PBI request poll (which
// then serves what this submitted).
void fakeSioPoll();
// End-of-run counters and verdict.
void fakeSioReport();
#endif
