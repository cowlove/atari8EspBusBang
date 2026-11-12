#include "csim.h"
#include "asmDefs.h"
#include "../../main/mmu.h"
#include "../../main/main.h"
#include "../../main/profile.h"
#include "diskFlash.h"
#include "../../main/pbi.h"
#include "../../main/bmon.h"
#include "../../main/const.h"
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>

#ifdef ATARI800_SIM_RUNNER
extern "C" {
uint8_t MEMORY_mem[65538] = {};
}
#endif

#ifdef PROFILE_ENABLE_ARRAY
const int Hist2::maxBucket;
#endif


// The bmon bits the fake bus cycles below carry: refresh_ high (not a
// refresh cycle), so the refresh filters pass them. No filter, and no
// refresh_, without HAVE_REFRESH (Sidecar builds).
#ifdef HAVE_REFRESH
static constexpr uint32_t kNotRefresh = bus.refresh_.mask;
#else
static constexpr uint32_t kNotRefresh = 0;
#endif

extern "C" CORE0_HOTSPOTS_ATTR void csim_pbi_control_block_write(uint16_t addr, void *request) {
#if 0 
	bmonArray[bmonHead].r0 = (addr << bus.addr.shift) | kNotRefresh;
	bmonHead = (bmonHead + 1) & bmonArraySzMask;
	core0_onBmonChange();
#else
	(void)addr;
	handlePbiRequest((PbiIocb *)request);
#endif
}

#if CONFIG_A8BB_SIDECAR_ENABLE
void csim_sidecar_tick();
#endif

extern "C" void csim_atari800_cpu_tick(uint32_t cycles) {
	csim_app_cpu_tick(cycles);
#if CONFIG_A8BB_SIDECAR_ENABLE
	csim_sidecar_tick();
#endif
}

extern "C" int csim_pbi_irq_pending(void) {
	return csim_app_pbi_irq_pending();
}

static int csimBusCycleTest();


extern "C" void csim_setup_with_ram(uint8_t *ram, size_t ramSize) {
	csim_app_setup_with_ram(ram, ramSize);
	if (getenv("CSIM_BUS_CYCLE_TEST"))
		exit(csimBusCycleTest() ? 1 : 0);
}

#ifdef CSIM_BUS_CYCLE
// Bus-cycle mode (make BUS_CYCLE=1): the atari800 fork's cpu.c runs every
// CPU access through here. The board comes up on the first access (the
// reset vector fetch), as it is up before the Atari on the hardware.
// Returns nonzero when the board asserted extSel_ and handled the access.
extern "C" uint8_t MEMORY_mem[65536 + 2];

// halt_, approximately. csim's GPIO_ENABLE1_W1TC/W1TS variables only hold
// the last mask written, so the pin's state is tracked here: each cycle
// releases (W1TC: pinReleaseMask) and then asserts (W1TS: the page's ctrl
// word), as core 1 does. While halt_ is asserted, atari800's cpu.c runs no
// instruction: it calls csim_cpu_halt_cycle() for an idle bus cycle and
// gives core 0 its tick, until core 0's release reaches the pin. That is
// enough to tell whether a halt stops the 6502 (memory autodetection's halt
// test); it doesn't make PSRAM swaps exact (core 0 still runs on the tick).
// CSIM_HALT_LINE=0 plays a machine without the line (a 130XE on the ECI):
// halt_ is driven but nothing stalls.
static bool csimBusUp = false;      // the board is set up; idle cycles allowed
static uint32_t csimEnable1 = 0;    // GPIO_ENABLE1 as the pins see it
static int csimHaltLine = -1;       // CSIM_HALT_LINE, read once

static int csimTrackedCycle(uint16_t addr, bool read, uint8_t *data) {
	const int took = csim_bus_cycle(addr, read, data);
	csimEnable1 = (csimEnable1 & ~GPIO_ENABLE1_W1TC_REG) | GPIO_ENABLE1_W1TS_REG;
	return took;
}

extern "C" int csim_cpu_bus_cycle(uint16_t addr, int read, uint8_t *data) {
	static bool started = false;
	if (!started) {
		started = true;
		csim_setup_with_ram(MEMORY_mem, 65536);
		csimBusUp = true;
	}
	return csimTrackedCycle(addr, read != 0, data);
}

static uint64_t csimHaltedCycles = 0, csimHaltStalls = 0;
static bool csimWasHalted = false;

extern "C" int csim_cpu_halted(void) {
	if (csimHaltLine < 0) {
		const char *e = getenv("CSIM_HALT_LINE");
		csimHaltLine = (e == nullptr || atoi(e) != 0) ? 1 : 0;
		atexit([] {
			printf("csim halt line %s: %llu stalls, %llu halted cycles\n",
			       csimHaltLine ? "on" : "off (CSIM_HALT_LINE=0)",
			       (unsigned long long)csimHaltStalls, (unsigned long long)csimHaltedCycles);
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
extern "C" void csim_cpu_halt_cycle(void) {
	uint8_t data;
	csimTrackedCycle(0xffff, true, &data);
}

// Core 0 as a coroutine (bus-cycle mode). On the hardware core 0 runs
// beside core 1; here atari800's CPU loop is core 1's clock, and core 0
// gets its own stack and runs between instructions: each CPU tick
// (main.cpp csim_app_cpu_tick()) resumes it, and it yields back whenever
// it waits, at a CCOUNT read every so often (csim.h) or in a busy wait.
// atari800 is never re-entered: core 0's state lives on its own stack.
#include <ucontext.h>
void csim_app_core0_pass();
static ucontext_t csimMainCtx, csimCore0Ctx;
static bool csimCore0Created = false;
bool csimInCore0 = false;   // csim.h: yield only from core 0's own stack
void csim_core0_yield();

static void csimCore0Entry() {
	for (;;) {
		csim_app_core0_pass();
		csim_core0_yield();
	}
}

void csim_core0_resume() {
	if (!csimCore0Created) {
		static const size_t stackSize = 1 << 20;
		getcontext(&csimCore0Ctx);
		csimCore0Ctx.uc_stack.ss_sp = malloc(stackSize);
		csimCore0Ctx.uc_stack.ss_size = stackSize;
		csimCore0Ctx.uc_link = &csimMainCtx;
		makecontext(&csimCore0Ctx, csimCore0Entry, 0);
		csimCore0Created = true;
	}
	csimInCore0 = true;
	swapcontext(&csimMainCtx, &csimCore0Ctx);
	csimInCore0 = false;
}

void csim_core0_yield() {
	if (csimInCore0)
		swapcontext(&csimCore0Ctx, &csimMainCtx);
}

// busyWait6502Ticks(): on core 0's stack, yield until that much emulated
// time has passed (the 6502 runs meanwhile, or sits halted). Elsewhere
// (before the coroutine starts) idle bus cycles, as core 1 would serve.
extern "C" void csim_busy_wait_6502_ticks(uint32_t ticks) {
	if (!csimBusUp)
		return;
	if (csimInCore0) {
		const uint64_t until = CSIM_PEEK_CCOUNT64() + uint64_t(ticks) * 132U;
		while (CSIM_PEEK_CCOUNT64() < until)
			csim_core0_yield();
		return;
	}
	for (uint32_t i = 0; i < ticks; i++)
		csim_cpu_halt_cycle();
}
#else
extern "C" void csim_busy_wait_6502_ticks(uint32_t ticks) { (void)ticks; }
#endif

#ifdef ATARI800_SIM_RUNNER
extern "C" void app_main(void);
int main() {
	app_main();
	return 0;
}
#endif

#include "bmon.h"
#include "pbi.h"
#include "main.h"

void csim_hook() { 
	static uint32_t now = 0, lastInterrupt = 0, lastIO = 0, lastStackProg = 0;

    PbiIocb *pbiRequest = (PbiIocb *)&pbiROM[0x30];
	static int intrPending = 0, ioPending = 0;
	now++;
	if (now - lastInterrupt > 200000 && ioPending + intrPending == 0) {
		intrPending = 1;
		bzero(&pbiRequest[0], sizeof(pbiRequest[0]));
		pbiRequest[0].cmd = 0x8;
		pbiRequest[0].req = REQ_FLAG_STACKWAIT;
		bmonArray[bmonHead].r0 = (0xd830 << bus.addr.shift) | kNotRefresh;
		bmonHead = (bmonHead + 1) & bmonArraySzMask;
		lastInterrupt = now;
	} 
	if(now - lastIO > 105000 && ioPending + intrPending == 0) {
		ioPending = 1;
		AtariDCB *dcb = atariMem.dcb;
		uint16_t addr = 0x4000;
		uint16_t len = 0x80;

		bzero(&pbiRequest[0], sizeof(pbiRequest[0]));
		bzero(dcb, sizeof(*dcb));

		dcb->DDEVIC = 0x31;
		dcb->DUNIT = 1;
		dcb->DSTATS = 0x40;
		dcb->DBUFHI = addr >> 8;
		dcb->DBUFLO = addr & 0xff;
		dcb->DBYTHI = len >> 8;
		dcb->DBYTLO = len & 0xff;
		dcb->DAUX1 = 1;
		dcb->DAUX2 = 1;
		
		pbiRequest[0].cmd = 0x7;
		pbiRequest[0].req = REQ_FLAG_STACKWAIT;
		bmonArray[bmonHead].r0 = (0xd830 << bus.addr.shift) | kNotRefresh;
		bmonHead = (bmonHead + 1) & bmonArraySzMask;
		lastIO = now;
	}

	if (intrPending + ioPending > 0) { 
		bmonArray[bmonHead].r0 = (0x0103 << bus.addr.shift) | kNotRefresh;
		bmonHead = (bmonHead + 1) & bmonArraySzMask;
	}

	if (pbiRequest[0].req == 0) { 
		intrPending = ioPending = 0;
	}
}

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

	bmonTail = bmonHead;
	printf("csim bus cycle test: %s\n", fails ? "FAILED" : "PASSED");
	return fails;
}

