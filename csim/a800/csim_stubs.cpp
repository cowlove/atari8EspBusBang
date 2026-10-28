#include "csim.h"
#include "asmDefs.h"
#include "../../main/mmu.h"
#include "../../main/main.h"
#include "../../main/profile.h"
#include "../../main/diskFlash.h"
#include "../../main/pbi.h"
#include "../../main/bmon.h"
#include "driver/i2s_std.h"
#include <dirent.h>
#include <sys/stat.h>

#ifdef ATARI800_SIM_RUNNER
extern "C" {
uint8_t MEMORY_mem[65538] = {};
}
#endif

uint8_t lastPageOffset[nrPages * (1 << PAGESEL_EXTRA_BITS)] = {};
const int Hist2::maxBucket;

extern "C" spiffs *spiffs_fs_by_label(const char *) { return nullptr; }

int SPIFFS_stat(spiffs_t *, const char *path, spiffs_stat *result) {
	struct stat info;
	if (stat(path, &info) != 0) return -1;
	if (result != nullptr) result->size = static_cast<size_t>(info.st_size);
	return 0;
}

spiffs_file SPIFFS_open(spiffs_t *, const char *path, int flags, int mode) {
	return open(path, flags, mode);
}

int SPIFFS_close(spiffs_t *, spiffs_file fd) { return close(fd); }
int SPIFFS_read(spiffs_t *, spiffs_file fd, void *buffer, int size) { return static_cast<int>(read(fd, buffer, size)); }
int SPIFFS_write(spiffs_t *, spiffs_file fd, const void *buffer, int size) { return static_cast<int>(write(fd, buffer, size)); }
int SPIFFS_lseek(spiffs_t *, spiffs_file fd, int offset, int whence) { return static_cast<int>(lseek(fd, offset, whence)); }
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

void DiskImageSMB::close() {}
int DiskImageSMB::sectorSize() { return 0; }
int DiskImageSMB::sectorCount() { return 0; }
size_t DiskImageSMB::read(uint8_t *, size_t) { return 0; }
int DiskImageSMB::write(const uint8_t *, size_t) { return 0; }

extern "C" esp_err_t i2sDmaPocSetDirectTxBuffer(i2s_chan_handle_t, uint8_t *, size_t)
{
    return ESP_OK;
}

extern "C" void i2sDmaPocDisableGdmaTxInterrupts(i2s_chan_handle_t) {}

extern "C" CORE0_HOTSPOTS_ATTR void csim_pbi_control_block_write(uint16_t addr, uint8_t byte) {
	(void)byte;
#if 0 
	bmonArray[bmonHead].r0 = (addr << bus.addr.shift) | bus.refresh_.mask;
	bmonHead = (bmonHead + 1) & bmonArraySzMask;
	core0_onBmonChange();
#else
	handlePbiRequest((PbiIocb *)&atariRam[addr]);
#endif
}

extern "C" void csim_atari800_cpu_tick(uint32_t cycles) {
	csim_app_cpu_tick(cycles);
}

extern "C" int csim_pbi_irq_pending(void) {
	return csim_app_pbi_irq_pending();
}

extern "C" void csim_setup_with_ram(uint8_t *ram, size_t ramSize) {
	csim_app_setup_with_ram(ram, ramSize);
}

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
		bmonArray[bmonHead].r0 = (0xd830 << bus.addr.shift) | bus.refresh_.mask;
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
		bmonArray[bmonHead].r0 = (0xd830 << bus.addr.shift) | bus.refresh_.mask;
		bmonHead = (bmonHead + 1) & bmonArraySzMask;
		lastIO = now;
	}

	if (intrPending + ioPending > 0) { 
		bmonArray[bmonHead].r0 = (0x0103 << bus.addr.shift) | bus.refresh_.mask;
		bmonHead = (bmonHead + 1) & bmonArraySzMask;
	}

	if (pbiRequest[0].req == 0) { 
		intrPending = ioPending = 0;
	}
}
