// CSIM: the board's side of the line between atari800 and the host app
// (include/csim_atari800.h, the atari800 side in csim/a800/atari800/src
// pbi.c and cpu.c). atari800 runs the 6502 and the Atari's chips; this
// file turns its calls into the board's: setup, the CPU tick that runs
// core 0 (in line in the default mode, a coroutine in bus-cycle mode), the
// PBI interrupt line, and, by mode, the PBI requests or every bus cycle
// through core 1's loop body with halt_ tracked. The app side it calls is
// main.h's csim_app_*(). Also here: the bus-cycle self-test
// (CSIM_BUS_CYCLE_TEST=1).
#include "csim.h"
#include "csim_atari800.h"
#include "csim_coroutine.h"
#include "asmDefs.h"
#include "../../main/mmu.h"
#include "../../main/main.h"
#include "../../main/pbi.h"
#include "../../main/bmon.h"
#include "../../main/const.h"
#include <stdlib.h>

// atari800's 64 KB (its memory.h), what the board comes up over.
extern "C" uint8_t MEMORY_mem[65536 + 2];

static int csimBusCycleTest();

extern "C" void CPU_CSIM_TraceDumpN(const char *why, unsigned int max);   // atari800's cpu.c
void csim_board_setup(uint8_t *ram, size_t ramSize) {
	// CSIM_TRACE_AT_EXIT=<n>: the 6502's last n instructions (up to 128)
	// when the run ends, e.g. to see where a wedged 6502 went.
#ifdef CSIM_BUS_CYCLE   // atari800's trace is bus-cycle mode's only
	static bool traceArmed = false;
	if (!traceArmed && getenv("CSIM_TRACE_AT_EXIT")) {
		traceArmed = true;
		atexit([] { CPU_CSIM_TraceDumpN("the run's end", atoi(getenv("CSIM_TRACE_AT_EXIT"))); });
	}
#endif
	csim_app_setup(ram, ramSize);
	if (getenv("CSIM_BUS_CYCLE_TEST"))
		exit(csimBusCycleTest() ? 1 : 0);
}

#ifdef CSIM_BUS_CYCLE
static int csimIrqPin(void);   // below: irq_ as core 1 drives it
#endif
#ifndef CSIM_BUS_CYCLE
void csim_core0_gpio_enable1(uint32_t, uint32_t) {}   // no per-cycle pins in this mode
#endif
int csim_board_irq(void) {
#ifdef CSIM_BUS_CYCLE
	return csimIrqPin();
#else
	return csim_app_irq();   // the default mode has no per-cycle pins: main's request flag
#endif
}

void csim_board_pbi_request(uint16_t addr, uint8_t *iocb) {
	(void)addr;
	handlePbiRequestHot((PbiIocb *)iocb);
}

#ifdef CSIM_BUS_CYCLE
void csim_core0_resume();
#endif
#if A8BB_SIDECAR
void csim_sidecar_tick();
#endif

// Machine cycles of atari800's clock as CCOUNT ticks (240 MHz): about 135
// (PAL) or 134 (NTSC) each. csim.h.
uint64_t csim_cycles_to_ccount(uint64_t cycles) {
	return cycles * 240000000ULL / csim_atari800_clock_hz();
}

// The 6502's tick (every 16 or more cycles, and each halted cycle). Once
// the board is up: advance the time, then run core 0. Bus-cycle mode
// resumes core 0's coroutine, which runs the real threadFunc()/core0Loop()
// until it waits (a CCOUNT read every so often, a busy wait) and yields
// back; the default mode runs csim_app_core0_pass(), csim's own pieces of
// core 0, in line. Then the Sidecar's turn.
bool csim_main_reboot_take();   // csim_atari_reset.cpp
#ifdef CSIM_BUS_CYCLE
static void csimMainReboot();
#endif

void csim_board_tick(uint32_t cycles) {
	// The Sidecar reset main (csim_atari_reset.cpp): reboot it here, on the
	// scheduler's stack, between CPU ticks.
	if (csim_main_reboot_take()) {
#ifdef CSIM_BUS_CYCLE
		csimMainReboot();
#else
		printf("csim: main reset ignored: the default mode can't reboot main (BUS_CYCLE=1 can)\n");
#endif
	}
	if (csim_app_setup_done()) {
		// The remainder carries, so the clocks don't drift apart.
		static uint64_t rem = 0;
		const uint64_t hz = csim_atari800_clock_hz();
		const uint64_t num = uint64_t(cycles) * 240000000ULL + rem;
		rem = num % hz;
		CSIM_ADVANCE_CCOUNT(static_cast<uint32_t>(num / hz));
#ifdef CSIM_BUS_CYCLE
		csim_core0_resume();
#else
		csim_app_core0_pass();
#endif
	}
#if A8BB_SIDECAR
	csim_sidecar_tick();
#endif
}

#ifdef CSIM_BUS_CYCLE
// Bus-cycle mode (make BUS_CYCLE=1): the atari800 fork's cpu.c runs every
// CPU access through csim_board_bus_cycle(). The board comes up on the first access (the
// reset vector fetch), as it is up before the Atari on the hardware.
// Returns nonzero when the board asserted extSel_ and handled the access.

// halt_, approximately. csim's GPIO_ENABLE1_W1TC/W1TS variables only hold
// the last mask written, so the pin's state is tracked here: each cycle
// releases (W1TC: pinReleaseMask) and then asserts (W1TS: the page's ctrl
// word), as core 1 does. While halt_ is asserted, atari800's cpu.c runs no
// instruction: it calls csim_board_halt_cycle() for an idle bus cycle and
// gives core 0 its tick, until core 0's release reaches the pin. That is
// enough to tell whether a halt stops the 6502 (memory autodetection's halt
// test); it doesn't make PSRAM swaps exact (core 0 still runs on the tick).
// CSIM_HALT_LINE=0 plays a machine without the line (a 130XE on the ECI):
// halt_ is driven but nothing stalls.
static bool csimBusUp = false;      // the board is set up; idle cycles allowed
static uint32_t csimEnable1 = 0;    // GPIO_ENABLE1 as the pins see it
static int csimHaltLine = -1;       // CSIM_HALT_LINE, read once

static uint16_t csimLastHaltAddr = 0;   // the access that last asserted halt_
static bool csimLastHaltRead = false;

static int csimTrackedCycle(uint16_t addr, bool read, uint8_t *data) {
	const int took = csim_bus_cycle(addr, read, data);
	if ((GPIO_ENABLE1_W1TS_REG & bus.halt_.mask) && !(csimEnable1 & bus.halt_.mask)) {
		csimLastHaltAddr = addr;
		csimLastHaltRead = read;
	}
	csimEnable1 = (csimEnable1 & ~GPIO_ENABLE1_W1TC_REG) | GPIO_ENABLE1_W1TS_REG;
	return took;
}

// A bus cycle core 0's FAKE_SIO test (main/fakeSio.cpp) makes in the
// 6502's place, between atari800's: a $D301 bank switch and its bank
// canaries. Tracked like the CPU's, so a halt_ it asserts stalls atari800.
int csim_fake_bus_cycle(uint16_t addr, bool read, uint8_t *data) {
	return csimTrackedCycle(addr, read, data);
}

// atari800's GTIA triggers (gtia.c). Its BASIC (no video) build never runs
// INPUT_Frame(), so TRIG0-2 stay 0, "pressed". The XL OS (AltirraOS at
// $EF0C) reads TRIG2 as the XEGS's keyboard-attached line: 0 there, at
// power-up, picks the built-in game over BASIC (PORTB $BF, BASIC off), so
// -basic never booted BASIC. Released, as on a stock XL. TRIG3 is the
// cartridge line, atari800's own.
extern "C" uint8_t GTIA_TRIG[4];
extern "C" uint8_t GTIA_TRIG_latch[4];

int csim_board_bus_cycle(uint16_t addr, int read, uint8_t *data) {
	static bool started = false;
	// CSIM_MAIN_START_MS=<ms>: the board comes up that long after the
	// Atari's power-on (a main that boots slower than the Atari, the worst
	// case on a machine without the halt line): until then it drives
	// nothing and atari800's own memory answers. Main's boot itself takes
	// no Atari time in csim; with the halt line, core 1 holds halt_ from
	// its first cycle.
	static uint64_t absentCycles = ~0ULL;
	if (absentCycles == ~0ULL) {
		const char *e = getenv("CSIM_MAIN_START_MS");
		absentCycles = e ? uint64_t(atoi(e)) * csim_atari800_clock_hz() / 1000 : 0;
	}
	if (!started && absentCycles > 0) {
		absentCycles--;
		if (absentCycles == 0)
			printf("csim: the board comes up (CSIM_MAIN_START_MS=%s)\n", getenv("CSIM_MAIN_START_MS"));
		return 0;
	}
	if (!started) {
		started = true;
		for (int i = 0; i < 3; i++)
			GTIA_TRIG[i] = GTIA_TRIG_latch[i] = 1;
		csim_board_setup(MEMORY_mem, 65536);
		csimBusUp = true;
	}
	return csimTrackedCycle(addr, read != 0, data);
}

// irq_, as halt_: the pin as the last bus cycle left it (core 1's W1TC
// release, then the page's W1TS assert: raiseInterrupt() puts it in every
// page's ctrl word and takes it out of pinReleaseMask; clearInterrupt()
// undoes both), not main's interruptRequested flag, so the 6502 sees the
// line when and only when the board's bus cycles drive it. CSIM_IRQ_PIN=0:
// the flag, as before. The exit report counts the 6502's samples where the
// two disagreed (transitions only: the pin follows the flag a cycle late).
// Core 0's own writes to GPIO_ENABLE1 (W1TS set, W1TC clear: main's
// irqJam()), as the pins see them at once: csim's register variables only
// hold the last value written, which core 1's next cycle overwrites.
void csim_core0_gpio_enable1(uint32_t set, uint32_t clear) {
	csimEnable1 = (csimEnable1 & ~clear) | set;
}

static int csimIrqFromPin = -1;   // CSIM_IRQ_PIN, read once
static uint64_t csimIrqSamples = 0, csimIrqDisagree = 0, csimIrqAsserted = 0;
static int csimIrqPin(void) {
	if (csimIrqFromPin < 0) {
		const char *e = getenv("CSIM_IRQ_PIN");
		csimIrqFromPin = (e == nullptr || atoi(e) != 0) ? 1 : 0;
		atexit([] {
			printf("csim irq_ %s: %llu of %llu samples asserted, %llu where pin and request flag differed%s\n",
			       csimIrqFromPin ? "from the pin" : "from the request flag (CSIM_IRQ_PIN=0)",
			       (unsigned long long)csimIrqAsserted, (unsigned long long)csimIrqSamples,
			       (unsigned long long)csimIrqDisagree, (csimEnable1 & bus.irq_.mask) ? ", still asserted" : "");
		});
	}
	const bool pin = (csimEnable1 & bus.irq_.mask) != 0, flag = csim_app_irq() != 0;
	const bool level = csimIrqFromPin ? pin : flag;
	csimIrqSamples++;
	if (level) csimIrqAsserted++;
	if (pin != flag) csimIrqDisagree++;
	return level;
}

static uint64_t csimHaltedCycles = 0, csimHaltStalls = 0;
static bool csimWasHalted = false;

int csim_board_halted(void) {
	if (csimHaltLine < 0) {
		const char *e = getenv("CSIM_HALT_LINE");
		csimHaltLine = (e == nullptr || atoi(e) != 0) ? 1 : 0;
		atexit([] {
			printf("csim halt line %s: %llu stalls, %llu halted cycles; last asserted by a %s of $%04x%s\n",
			       csimHaltLine ? "on" : "off (CSIM_HALT_LINE=0)",
			       (unsigned long long)csimHaltStalls, (unsigned long long)csimHaltedCycles,
			       csimLastHaltRead ? "read" : "write", csimLastHaltAddr,
			       (csimEnable1 & bus.halt_.mask) ? ", still held" : "");
		});
	}
	const bool halted = csimHaltLine && (csimEnable1 & bus.halt_.mask) != 0;
	if (halted) {
		csimHaltedCycles++;
		if (!csimWasHalted) csimHaltStalls++;
	}
	csimWasHalted = halted;
	return halted;
}

// One bus cycle with the 6502 halted: a read the board serves or ignores
// (bmon records only writes), so core 1's release and assert still run.
void csim_board_halt_cycle(void) {
	if (!csimBusUp)
		return;   // the board not up yet (CSIM_MAIN_START_MS): nothing to clock
	uint8_t data;
	csimTrackedCycle(0xffff, true, &data);
}

// Core 0 as a coroutine (bus-cycle mode). On the hardware core 0 runs
// beside core 1; here atari800's CPU loop is core 1's clock, and core 0
// gets its own stack (csim_coroutine.h) and runs between instructions:
// each CPU tick (csim_board_tick()) resumes it, and it yields
// back whenever it waits, at a CCOUNT read every 64 (csim.h) or in a busy
// wait or a link wait. atari800 is never re-entered: core 0's state lives
// on its own stack.
void threadFunc(void *);

// Core 0's code, as on the hardware: threadFunc() runs core0Loop() (bmon,
// PBI requests, utility tasks, timeouts) until the run ends, then prints
// its end-of-run report.
static void csimCore0Entry() {
	threadFunc(nullptr);
	csim_app_finish();
}

static CsimCoroutine csimCore0("core0", csimCore0Entry, 64);

void csim_core0_resume() {
	csimCore0.resume();
}

// A coarse main reboot (csim_atari_reset.cpp): core 0's coroutine starts
// over (threadFunc() from the top once setup() is done), and setup() runs
// again over main's state as it is.
static void csimMainReboot() {
	csimCore0.restart();
	csim_app_reboot();
}

static inline bool csimInCore0() {
	return csimCurrentCoroutine == &csimCore0;
}

// CSIM_6502_YIELD() and CSIM_CORE0_YIELD() (csim.h). Core 0 marks itself
// idle when it yields with nothing to do (CSIM_CORE0_YIELD(): bmon empty);
// a bus cycle that leaves bmon empty (a read) then doesn't resume it, as it
// would only poll once and yield back. Core 0 busy (yielded at a clock
// read) is resumed after every cycle, and the tick resumes it either way,
// so its timeouts still run.
static bool csimCore0Idle = false;

void csim_6502_yield() {
	if (csimCore0Idle && bmonHead == bmonTail)
		return;
	if (csimCore0.started() && !csimCore0.finished() && !csimInCore0())
		csimCore0.resume();
}

void csim_core0_yield() {
	if (!csimInCore0())
		return;
	csimCore0Idle = true;
	csimCore0.yield();
	csimCore0Idle = false;
}

// busyWait6502Ticks(): on core 0's stack, yield until that much emulated
// time has passed (the 6502 runs meanwhile, or sits halted). Elsewhere
// (before the coroutine starts) idle bus cycles, as core 1 would serve.
extern "C" void csim_busy_wait_6502_ticks(uint32_t ticks) {
	if (!csimBusUp)
		return;
	if (csimInCore0()) {
		const uint64_t until = CSIM_PEEK_CCOUNT64() + csim_cycles_to_ccount(ticks);
		while (CSIM_PEEK_CCOUNT64() < until)
			csimCore0.yield();
		return;
	}
	for (uint32_t i = 0; i < ticks; i++)
		csim_board_halt_cycle();
}
#else
extern "C" void csim_busy_wait_6502_ticks(uint32_t ticks) { (void)ticks; }
#endif

// CSIM_BUS_CYCLE_TEST=1: once setup is done, run bus cycles through
// csim_bus_cycle() (main/core1.cpp), check them against the MMU's map,
// print which pages this board answers (asserts extSel_ on a read), and
// exit (status 1 on a failure).
static int csimBusCycleTest() {
	int fails = 0;
	auto check = [&](bool ok, const char *what) {
		printf("csim bus cycle test: %s %s\n", ok ? "ok  " : "FAIL", what);
		if (!ok) fails++;
	};
	auto read = [](uint16_t addr, uint8_t *data) { return csim_bus_cycle(addr, true, data); };
	auto write = [](uint16_t addr, uint8_t data) { csim_bus_cycle(addr, false, &data); };

	// Which pages the board answers (E: extSel_ asserted on a read).
	char map[257] = {};
	for (int page = 0; page < 256; page++) {
		uint8_t data = 0;
		map[page] = read(page << 8, &data) ? 'E' : '.';
	}
	for (int row = 0; row < 4; row++)
		printf("csim bus cycle test: pages %02x-%02x %.64s\n", row * 64, row * 64 + 63, map + row * 64);

	// A RAM write lands where the MMU maps it and reads back.
	const uint16_t ramAddr = 0x0600;
	const uint8_t saved = atariRam[ramAddr];
	write(ramAddr, 0x5a);
	uint8_t data = 0;
	const bool handled = read(ramAddr, &data);
	check(atariRam[ramAddr] == 0x5a, "write to $0600 lands in atariRam");
	check(handled && data == 0x5a, "read of $0600 asserts extSel_ and returns the write");
	write(ramAddr, saved);

	// PBI select ($D1FF) maps the PBI ROM at $D800 on the next read cycle.
	const uint8_t savedD1ff = d000Write[_0x1ff];
	write(0xd1ff, pbiDeviceNumMask);
	read(0x0600, &data);
	check(mmuBlock(mmuState, l1BlockC0) ==
	      mmuState.osEnL1BlockMux[(d000Write[_0x301] & 1) | pbiDeviceNumMask],
	      "$D1FF write selects the PBI ROM block");
	check(read(0xd800, &data) && data == pbiROM[0], "read of $D800 returns pbiROM[0]");
	write(0xd1ff, savedD1ff);
	read(0x0600, &data);

	// PORTB ($D301) selects the extended memory bank at $4000.
	int bank = -1;
	for (int b = 1; b < 32 && bank < 0; b++)
		if (mmuState.extBanks[b << 2] != NULL && mmuState.extBanks[b << 2] != mmuState.extBanks[0])
			bank = b;
	if (bank < 0) {
		printf("csim bus cycle test: skip no extended memory banks configured\n");
	} else {
		const uint8_t savedPortb = d000Write[_0x301];
		write(0xd301, (savedPortb & ~0x7c) | (bank << 2));
		read(0x0600, &data);
		check(mmuBlock(mmuState, l1Block40) == mmuState.extBanks[(savedPortb & ~0x7c) | (bank << 2)],
		      "$D301 write selects an extended bank");
		write(0xd301, savedPortb);
		read(0x0600, &data);
		check(mmuBlock(mmuState, l1Block40) == mmuState.extBanks[savedPortb],
		      "$D301 restore selects the original bank");
	}

	// Base RAM allocate and tear down (mmuAllocAddBaseRam() /
	// mmuFreeUnmapBaseRam()): 8K at $4000 in pieces, plus a snapshot copy
	// of the $4000 block reachable from the MMU's tables, as the bank and
	// cartridge blocks are. The teardown must clear the pieces' pages from
	// both, and native RAM must answer afterwards.
	{
		static L1Block copy;
		uint8_t *pagesBefore[0x20];
		mmuAllocAddBaseRam(0x4000, 0x5fff);
		for (int p = 0; p < 0x20; p++) pagesBefore[p] = baseMemPages[0x40 + p];
		copy = l1Blocks[l1Block40];
		L1Block *const savedCart = mmuState.cartBanks[31];
		mmuState.cartBanks[31] = &copy;
		write(0x4123, 0xa7);
		const bool mapped = read(0x4123, &data) && data == 0xa7;
		check(mapped && pagesBefore[0] != nullptr && pagesBefore[0x1f] != nullptr,
		      "mmuAllocAddBaseRam($4000-$5FFF) maps it");
		mmuFreeUnmapBaseRam(0x4000, 0x5fff);
		bool clean = true;
		for (const L1Block *b : {(const L1Block *)&l1Blocks[l1Block40], (const L1Block *)&copy})
			for (uint8_t *page : b->pages)
				for (int p = 0; p < 0x20; p++)
					if (page >= pagesBefore[p] && page < pagesBefore[p] + pageSize) clean = false;
		bool cleared = true;
		for (int p = 0x40; p < 0x60; p++) if (baseMemPages[p] != nullptr) cleared = false;
		check(clean, "mmuFreeUnmapBaseRam() clears the pages from l1Blocks[] and a copy");
		check(cleared, "mmuFreeUnmapBaseRam() clears baseMemPages[]");
		check(!read(0x4123, &data), "after the teardown, $4000 is native (no extSel_)");
		mmuState.cartBanks[31] = savedCart;
	}

	bmonTail = bmonHead;
	printf("csim bus cycle test: %s\n", fails ? "FAILED" : "PASSED");
	return fails;
}


