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

extern "C" spiffs *spiffs_fs_by_label(const char *) { return nullptr; }

int SPIFFS_stat(spiffs_t *, const char *path, spiffs_stat *result) {
	struct stat info;
	if (stat(path, &info) != 0) {
		if (errno != ENOENT || path == nullptr) return -1;
		string fallbackPath = "../../spiffs/";
		fallbackPath.append(path[0] == '/' ? path + 1 : path);
		if (stat(fallbackPath.c_str(), &info) != 0) return -1;
	}
	if (result != nullptr) result->size = static_cast<size_t>(info.st_size);
	return 0;
}

spiffs_file SPIFFS_open(spiffs_t *, const char *path, int flags, int mode) {
	spiffs_file fd = open(path, flags, mode);
	if (fd >= 0 || errno != ENOENT || path == nullptr) return fd;

	string fallbackPath = "../../spiffs/";
	fallbackPath.append(path[0] == '/' ? path + 1 : path);
	return open(fallbackPath.c_str(), flags, mode);
}

int SPIFFS_close(spiffs_t *, spiffs_file fd) { return close(fd); }
int SPIFFS_read(spiffs_t *, spiffs_file fd, void *buffer, int size) { return static_cast<int>(read(fd, buffer, size)); }
int SPIFFS_write(spiffs_t *, spiffs_file fd, const void *buffer, int size) { return static_cast<int>(write(fd, buffer, size)); }
int SPIFFS_lseek(spiffs_t *, spiffs_file fd, int offset, int whence) { return static_cast<int>(lseek(fd, offset, whence)); }
int SPIFFS_errno(spiffs_t *) { return -errno; }
int SPIFFS_remove(spiffs_t *, const char *path) { return unlink(path); }
int SPIFFS_rename(spiffs_t *, const char *oldPath, const char *newPath) { return rename(oldPath, newPath); }
int SPIFFS_ftruncate(spiffs_t *, spiffs_file fd, uint32_t length) { return ftruncate(fd, length); }
spiffs_DIR *SPIFFS_opendir(spiffs_t *, const char *path, spiffs_DIR *dir) {
	const char *directory = path == nullptr || path[0] == '\0' ? "." : path;
	DIR *handle = opendir(directory);
	if (handle == nullptr) return nullptr;
	dir->handle = handle;
	strncpy(dir->path, directory, sizeof(dir->path) - 1);
	dir->path[sizeof(dir->path) - 1] = '\0';
	return dir;
}
spiffs_dirent *SPIFFS_readdir(spiffs_DIR *dir, spiffs_dirent *entry) {
	DIR *handle = static_cast<DIR *>(dir->handle);
	struct dirent *nativeEntry = handle == nullptr ? nullptr : ::readdir(handle);
	if (nativeEntry == nullptr) return nullptr;
	strncpy(reinterpret_cast<char *>(entry->name), nativeEntry->d_name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	char path[sizeof(dir->path) + sizeof(entry->name) + 1];
	snprintf(path, sizeof(path), "%s/%s", dir->path, nativeEntry->d_name);
	struct stat info;
	if (stat(path, &info) != 0) return nullptr;
	entry->type = S_ISDIR(info.st_mode) ? SPIFFS_TYPE_DIR : SPIFFS_TYPE_FILE;
	entry->size = S_ISREG(info.st_mode) ? static_cast<uint32_t>(info.st_size) : 0;
	return entry;
}
void SPIFFS_closedir(spiffs_DIR *dir) {
	DIR *handle = static_cast<DIR *>(dir->handle);
	if (handle != nullptr) closedir(handle);
	dir->handle = nullptr;
}

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
extern "C" int csim_cpu_bus_cycle(uint16_t addr, int read, uint8_t *data) {
	static bool started = false;
	if (!started) {
		started = true;
		csim_setup_with_ram(MEMORY_mem, 65536);
	}
	return csim_bus_cycle(addr, read != 0, data);
}
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

// CSIM_BUS_CYCLE_TEST=1: once setup is done, run bus cycles through core 1's
// iloop_pbi_cycle() (csim_bus_cycle()), check them against the MMU's map,
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
	check(mmuState.l1Blocks[l1BlockC0] ==
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
		check(mmuState.l1Blocks[l1Block40] == mmuState.extBanks[(savedPortb & ~0x7c) | (bank << 2)],
		      "$D301 write selects an extended bank");
		write(0xd301, savedPortb);
		read(0x0600, &data);
		check(mmuState.l1Blocks[l1Block40] == mmuState.extBanks[savedPortb],
		      "$D301 restore selects the original bank");
	}

	bmonTail = bmonHead;
	printf("csim bus cycle test: %s\n", fails ? "FAILED" : "PASSED");
	return fails;
}

