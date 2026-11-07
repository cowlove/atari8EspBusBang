// The Sidecar app: see sidecarApp.h.

#include "sidecarApp.h"

#include "sidecar.h"
#include "diskFlash.h"   // DiskImageATR (components/disk_image)

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"

#include <stdio.h>

// The raw SPIFFS handle behind the VFS mount (a local ESP-IDF addition,
// also stubbed in csim), as the host gets it in main.cpp.
extern "C" spiffs *spiffs_fs_by_label(const char *label);

namespace {

[[maybe_unused]] const char *TAG = "sidecar";   // ESP_LOGI compiles away under CSIM

// Commands handled, by type (cumulative).
struct Counts {
    uint32_t nop, sio, cio;
};
Counts counts = {};

class AtariService : public SidecarService {
protected:
    void handleNop() override { ++counts.nop; }

    void handleSio(const SioCommand &command, SioResponse &response) override
    {
        // TODO: perform command.body (AtariDCB) using command.data; put the
        // result status in response.body.DSTATS and any read data in
        // response.data.
        ++counts.sio;
        (void)command;
        (void)response;
    }

    void handleCio(const CioCommand &command, CioResponse &response) override
    {
        // TODO: perform command.body (AtariIOCB) using command.data; put
        // the result status in response.body.ICSTA and any read data in
        // response.data.
        ++counts.cio;
        (void)command;
        (void)response;
    }
};

AtariService service;

// TEST: a disk image from the Sidecar's own flash. The Sidecar carries the
// host's spiffs.bin (same partition layout), so this is the host's D2:.
constexpr const char *kTestImage = "/d2.atr.gz";
DiskImage *testDisk = nullptr;

// Mount SPIFFS, open kTestImage, and print its geometry, the start of
// sector 1 and a checksum over every sector (FNV-1a), for comparing with
// the host's copy.
void diskSelfTest()
{
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = "/spiffs";
    conf.partition_label = nullptr;
    conf.max_files = 5;
    conf.format_if_mount_failed = false;
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        printf("Sidecar disk test: SPIFFS mount failed (%s)\n", esp_err_to_name(err));
        return;
    }
    spiffs *fs = spiffs_fs_by_label(nullptr);
    DiskImageATR *disk = new DiskImageATR(fs, kTestImage, true);
    if (!disk->valid()) {
        printf("Sidecar disk test: '%s' did not open\n", kTestImage);
        delete disk;
        return;
    }
    uint8_t sector[256];
    uint32_t hash = 2166136261u;
    int bad = 0;
    for (int n = 1; n <= disk->sectorCount(); ++n) {
        const int size = disk->specificSectorSize(n);
        if (size > (int)sizeof(sector) || (int)disk->read(sector, n) <= 0) {
            ++bad;
            continue;
        }
        for (int i = 0; i < size; ++i) {
            hash = (hash ^ sector[i]) * 16777619u;
        }
    }
    disk->read(sector, 1);
    printf("Sidecar disk test: '%s': %d sectors of %d bytes, %d unreadable, FNV-1a %08x; "
           "sector 1: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           kTestImage, disk->sectorCount(), disk->sectorSize(), bad, (unsigned)hash, sector[0],
           sector[1], sector[2], sector[3], sector[4], sector[5], sector[6], sector[7]);
    testDisk = disk;   // kept open, for SIO later
}

int64_t nextLog = 0;
Counts lastCounts = {};
i2slink_stats_t lastStats = {};

}  // namespace

// The Sidecar follows the host's clock: the host drives BCLK, and every
// wire has one driver in all modes, including the host's loopback test
// (CONFIG_A8BB_SIDECAR_LOOPBACK), so a wired pair is safe either way.
const i2slink_config_t sidecarLinkConfig = {
    .clock = I2SLINK_CLOCK_FOLLOWER,
    .i2s_port = 0,         // I2S_NUM_0
    .bclk_gpio = 36,       // from the host's BCLK
    .tx_gpio = 35,         // to the host's RX
    .rx_gpio = 37,         // from the host's TX
    .rx_bclk_invert = true,
    .sync_timeout_us = 0,  // default: four wire cycles
    .loopback = false,
};

void sidecarAppBegin()
{
    diskSelfTest();
    ESP_ERROR_CHECK(service.begin(sidecarLinkConfig));
    nextLog = esp_timer_get_time() + 1000000;
}

void sidecarAppPoll()
{
    // Other periodic Sidecar work goes here, alongside service().
    service.service();
    const int64_t now = esp_timer_get_time();
    if (now >= nextLog) {
        nextLog = now + 1000000;
        // Per-second deltas, then totals.
        i2slink_stats_t s;
        i2slink_stats(service.link(), &s);
#define D(f) (unsigned)(s.f - lastStats.f)
#define C(f) (unsigned)(counts.f - lastCounts.f)
        ESP_LOGI(TAG, "1s: nop=%u sio=%u cio=%u | frames rx=%u tx=%u | reads{timeout=%u "
                 "torn=%u crc=%u nolock=%u stalled=%u} framing=%u jumps=%u | total nop=%u "
                 "sio=%u cio=%u rx=%u tx=%u locks=%u",
                 C(nop), C(sio), C(cio), D(reads_new), D(writes), D(reads_timeout),
                 D(reads_torn), D(reads_crc), D(reads_no_lock), D(reads_stalled), D(framing),
                 D(jumps), (unsigned)counts.nop, (unsigned)counts.sio, (unsigned)counts.cio,
                 (unsigned)s.reads_new, (unsigned)s.writes, (unsigned)s.locks);
#undef D
#undef C
        lastStats = s;
        lastCounts = counts;
    }
}
