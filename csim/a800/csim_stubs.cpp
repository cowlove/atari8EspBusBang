#include "csim.h"
#include "../../main/mmu.h"
#include "../../main/profile.h"
#include "../../main/diskFlash.h"
#include "driver/i2s_std.h"

uint8_t lastPageOffset[nrPages * (1 << PAGESEL_EXTRA_BITS)] = {};
const int Hist2::maxBucket;

extern "C" spiffs *spiffs_fs_by_label(const char *) { return nullptr; }

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
extern "C" void app_main(void);
int main() {
	app_main();
	return 0;
}
