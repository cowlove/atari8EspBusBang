// CSIM: odds and ends of the simulator's host side that aren't the
// atari800 interface (csim_atari800.cpp): the standalone runner's entry
// point and memory, and its fake PBI traffic (csim_hook()).
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
// refresh_, without HAVE_REFRESH (A8BB_REFRESH_GPIO 0).
#ifdef HAVE_REFRESH
static constexpr uint32_t kNotRefresh = bus.refresh_.mask;
#else
static constexpr uint32_t kNotRefresh = 0;
#endif

#ifdef ATARI800_SIM_RUNNER
// No atari800 here: an NTSC machine's clock for the board's tick.
extern "C" unsigned long csim_atari800_clock_hz(void) { return 1789790UL; }

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
		bmonArray[bmonHead & bmonArraySzMask].r0 = addrToR0(0xd830) | kNotRefresh;
		bmonHead = bmonHead + 1;
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
		bmonArray[bmonHead & bmonArraySzMask].r0 = addrToR0(0xd830) | kNotRefresh;
		bmonHead = bmonHead + 1;
		lastIO = now;
	}

	if (intrPending + ioPending > 0) { 
		bmonArray[bmonHead & bmonArraySzMask].r0 = addrToR0(0x0103) | kNotRefresh;
		bmonHead = bmonHead + 1;
	}

	if (pbiRequest[0].req == 0) { 
		intrPending = ioPending = 0;
	}
}

