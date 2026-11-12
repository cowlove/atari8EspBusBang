// The Sidecar app: see sidecarApp.h.

#include "sidecarApp.h"

#include "sidecar.h"
#include "diskStitchProc.h"   // ProcFsFile (components/disk_image)
#include "sio.h"   // Sio, DiskImage (components/disk_image)

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#ifndef CSIM
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#endif

#include <stdio.h>
#include <string.h>

// The raw SPIFFS handle behind the VFS mount (a local ESP-IDF addition,
// also stubbed in csim), as the host gets it in main.cpp.
extern "C" spiffs *spiffs_fs_by_label(const char *label);

#ifdef CSIM
// csim runs this app inside the host's process, so the Sidecar's HALT file
// can end the run the way the host's does (main.cpp).
extern std::string exitReason;
extern int exitFlag;
#endif

namespace {

[[maybe_unused]] const char *TAG = "sidecar";   // ESP_LOGI compiles away under CSIM

// Commands handled, by type (cumulative).
struct Counts {
    uint32_t nop, sio, cio, mount;
};
Counts counts = {};

// The Sidecar's drives. It carries the host's spiffs.bin (same partition
// layout), so the same diskSpecs name the same images.
Sio sio;

// The Sidecar's own "proc:" disk files, distinct from the host's
// (main/procFsNodes.h).
const vector<ProcFsFile> procFsNodes = {
    {   .name = "LOG",
        .read = [](string &buf) {},
        .write = [](const string &s) { printf("LOG: '%s'\n", s.c_str()); },
    },
#ifdef CSIM
    {   .name = "HALT",
        .write = [](const string &s) {
            printf("LOG: HALT written '%s'\n", s.c_str());
            exitReason = "Sidecar ProcFS HALT file written :" + s;
            exitFlag = -5;
        },
    },
#endif
};

class AtariService : public SidecarService {
protected:
    void handleNop() override { ++counts.nop; }

    // An SIO DCB against this Sidecar's own drives. Mirrors the host's
    // local handling (handlePbiRequest2() in main/pbi.cpp): the same
    // commands and results, with the data in the message instead of Atari
    // memory. The response starts as a copy of the command, status 0.
    void handleSio(const SioCommand &command, SioResponse &response) override
    {
        ++counts.sio;
        const AtariDCB &dcb = command.body;
        // Report slow commands: the host gives up after 6 s (kSioTimeoutUs
        // in main/sidecarHost.cpp) and shows the Atari 138.
        struct SlowReport {
            const AtariDCB &dcb;
            const SioResponse &response;
            const int64_t start = esp_timer_get_time();
            ~SlowReport() {
                const int64_t us = esp_timer_get_time() - start;
                if (us > 50000) {
                    printf("Sidecar: D%d: cmd %02x sector %d took %lld us (status %d, DSTATS %02x)\n",
                           dcb.DUNIT, dcb.DCOMND, (dcb.DAUX2 << 8) | dcb.DAUX1, (long long)us,
                           response.header.status, response.body.DSTATS);
                }
            }
        } slowReport{dcb, response};
        if ((dcb.DDEVIC & 0x7f) != 0x31 || dcb.DUNIT < 1 || dcb.DUNIT > Sio::kDrives) {
            return;   // not one of our disks: fail, DSTATS unchanged
        }
        DiskImage *disk = sio.disk(dcb.DUNIT - 1);
        if (disk == nullptr || !disk->valid()) {
            return;   // no disk: fail (not ready)
        }
        const int sectorSize = disk->sectorSize();
        const int sector = (dcb.DAUX2 << 8) | dcb.DAUX1;
        uint8_t *data = response.data;
        auto succeed = [&response]() {
            response.body.DSTATS = 0x01;
            response.header.status = 1;
        };
        switch (dcb.DCOMND) {
        case 0x53:   // status (https://www.atarimax.com/jindroush.atari.org/asio.html)
            data[0] = sectorSize != 128 ? 0x20 : 0x00;   // sector size bit
            data[1] = 0xff;   // inverted: busy, DRQ, lost, crc, rnf, head, wp, not ready
            data[2] = 0xff;   // format timeout
            data[3] = 0xff;   // copy of the WD status
            succeed();
            break;
        case 0x52:
        case 0xd2:   // read sector
            if (disk->specificSectorSize(sector) > (int)sizeof(response.data)) {
                return;
            }
            // A short or failed read must fail the command, not return
            // whatever is left in response.data.
            if (const size_t got = disk->read(data, sector);
                got != (size_t)disk->specificSectorSize(sector)) {
                spiffs *fs = spiffs_fs_by_label(nullptr);
                printf("Sidecar: D%d: read sector %d failed: got %d of %d (SPIFFS err %d)\n",
                       dcb.DUNIT, sector, (int)got, disk->specificSectorSize(sector),
                       fs ? (int)SPIFFS_errno(fs) : 0);
                response.body.DSTATS = 0;
                return;
            }
            succeed();
            break;
        case 0x50:
        case 0xd0:
        case 0x57:   // write sector (verify ignored)
            if (const int wrote = disk->write(command.data, sector); wrote < 0) {
                spiffs *fs = spiffs_fs_by_label(nullptr);
                printf("Sidecar: D%d: write sector %d failed: %d (SPIFFS err %d)\n",
                       dcb.DUNIT, sector, wrote, fs ? (int)SPIFFS_errno(fs) : 0);
                response.body.DSTATS = 0;
                return;
            }
            succeed();
            break;
        case 0x3f:   // high-speed capabilities
            data[0] = 0x28;
            succeed();
            break;
        case 0x48:   // Happy command
            succeed();
            break;
        case 0x4e: {   // read the PERCOM block
            const int sectors = disk->sectorCount();
            const uint8_t percom[12] = {
                1,   // tracks
                3,   // step rate
                (uint8_t)(sectors >> 8), (uint8_t)sectors,   // sectors per track
                1,   // sides
                4,   // density (MFM)
                (uint8_t)(sectorSize >> 8), (uint8_t)sectorSize,
                0xff,   // drive online
                0, 0, 0,
            };
            memcpy(data, percom, sizeof(percom));
            succeed();
            break;
        }
        case 0x4f:   // write the PERCOM block (ignored)
            succeed();
            break;
        default:
            break;   // unknown command: fail
        }
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

    void handleMount(const MountCommand &command, MountResponse &response) override
    {
        ++counts.mount;
        const int unit = command.body.unit;
        const char *spec = reinterpret_cast<const char *>(command.data);
        SidecarMountStatus status;
        if (unit < 1 || unit > Sio::kDrives ||
            memchr(spec, 0, sizeof(command.data)) == nullptr) {
            status = SidecarMountStatus::BadRequest;
        } else {
            status = sio.mount(unit - 1, spec) ? SidecarMountStatus::Mounted
                                               : SidecarMountStatus::Failed;
        }
        printf("Sidecar mount: D%d: '%.*s': %s\n", unit, (int)sizeof(command.data), spec,
               status == SidecarMountStatus::Mounted  ? "mounted"
               : status == SidecarMountStatus::Failed ? "failed"
                                                      : "bad request");
        response.header.status = static_cast<uint8_t>(status);
    }
};

AtariService service;

// Mount the Sidecar's SPIFFS (the host's spiffs.bin) and hand it to sio,
// for the diskSpecs the host sends in Mount commands.
void mountSpiffs()
{
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = "/spiffs";
    conf.partition_label = nullptr;
    conf.max_files = 5;
    conf.format_if_mount_failed = false;
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        printf("Sidecar: SPIFFS mount failed (%s); SPIFFS diskSpecs won't mount\n",
               esp_err_to_name(err));
        return;
    }
    sio.setFs(spiffs_fs_by_label(nullptr));
}

#ifndef CSIM
// Optional microSD card, 4-bit SDMMC through the GPIO matrix, on six
// adjacent pins (DevKitC-1 J3-4..9) in the socket's pin order; see
// docs/sidecar_wiring.pdf. The pins avoid the strapping, USB, UART0,
// flash/PSRAM, RGB LED, link and bus-profiler pins. CMD and DAT0-3 need
// 10 k external pull-ups; the internal ones are enabled too but are too
// weak alone.
constexpr gpio_num_t kSdClk = GPIO_NUM_41, kSdCmd = GPIO_NUM_42, kSdD0 = GPIO_NUM_40,
                     kSdD1 = GPIO_NUM_39, kSdD2 = GPIO_NUM_1, kSdD3 = GPIO_NUM_2;
sdmmc_card_t *sdCard = nullptr;

esp_err_t trySdMount(int maxFreqKhz)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = maxFreqKhz;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = kSdClk;
    slot.cmd = kSdCmd;
    slot.d0 = kSdD0;
    slot.d1 = kSdD1;
    slot.d2 = kSdD2;
    slot.d3 = kSdD3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;   // never reformat a user's card
    mount.max_files = 5;
    return esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mount, &sdCard);
}

// Mount the card's FAT volume at /sd if there is one. No card, a card that
// won't initialize or one without a FAT volume is fine: the Sidecar just
// runs without it.
void mountSdCard()
{
    const int64_t start = esp_timer_get_time();
    esp_err_t err = trySdMount(SDMMC_FREQ_HIGHSPEED);
    // No card shows up as a timeout; anything else may be the 40 MHz clock
    // (long wires, a slow card), so try once more at the default 20 MHz.
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
        err = trySdMount(SDMMC_FREQ_DEFAULT);
    const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        sdCard = nullptr;
        printf("Sidecar: no SD card (%s, %d ms); continuing without it\n",
               esp_err_to_name(err), ms);
        return;
    }
    const uint64_t mb = uint64_t(sdCard->csd.capacity) * sdCard->csd.sector_size >> 20;
    printf("Sidecar: SD card '%s' mounted at /sd: %llu MB, %d kHz, %d-bit (%d ms)\n",
           sdCard->cid.name, static_cast<unsigned long long>(mb), sdCard->real_freq_khz,
           1 << sdCard->log_bus_width, ms);
}
#endif

int64_t nextLog = 0;
Counts lastCounts = {};
i2slink_stats_t lastStats = {};

#ifndef CSIM
// Idle garbage collection. SPIFFS collects inline when a write runs short of
// free blocks, inside the host's SIO wait (seen at 2.6 s). Once the link has
// been quiet for kIdleGcAfterUs, erase the blocks that hold only deleted
// pages ahead of time, one per pass (a block erase, tens of ms) so a command
// that arrives meanwhile waits for one erase at most. A pass that finds none
// ends the run until the next command. SPIFFS_gc_quick() with 0 never moves
// live pages, so it costs no extra flash wear.
constexpr int64_t kIdleGcAfterUs = 3000000;
constexpr int64_t kIdleGcIntervalUs = 100000;
int64_t lastCommandUs = 0;
int64_t nextGcUs = 0;
bool gcIdle = false;     // true once a run found nothing left to erase
unsigned gcBlocks = 0;   // erased in this run
int64_t gcBusyUs = 0;    // time spent in SPIFFS_gc_quick() in this run

void idleGc(int64_t now)
{
    if (gcIdle || now - lastCommandUs < kIdleGcAfterUs || now < nextGcUs) {
        return;
    }
    spiffs *fs = spiffs_fs_by_label(nullptr);
    if (fs == nullptr) {
        gcIdle = true;
        return;
    }
    const int64_t start = esp_timer_get_time();
    const s32_t res = SPIFFS_gc_quick(fs, 0);
    const int64_t end = esp_timer_get_time();
    gcBusyUs += end - start;
    nextGcUs = end + kIdleGcIntervalUs;
    if (res == SPIFFS_OK) {
        ++gcBlocks;
        return;
    }
    gcIdle = true;
    if (res != SPIFFS_ERR_NO_DELETED_BLOCKS) {
        printf("Sidecar: idle GC failed (SPIFFS err %d)\n", (int)res);
    }
    if (gcBlocks > 0) {
        u32_t total = 0, used = 0;
        SPIFFS_info(fs, &total, &used);
        printf("Sidecar: idle GC erased %u blocks in %lld ms; %u of %u KB used\n", gcBlocks,
               (long long)(gcBusyUs / 1000), (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
    gcBlocks = 0;
    gcBusyUs = 0;
}
#endif

}  // namespace

// The Sidecar follows the host's clock: the host drives BCLK, and every
// wire has one driver in all modes, including the host's loopback test
// (CONFIG_A8BB_SIDECAR_LOOPBACK), so a wired pair is safe either way.
//
// Pins: TX GPIO46, RX GPIO9, BCLK GPIO10, three adjacent pins on a
// DevKitC-1's J1 header (pins 14, 15 and 16: ... 3, 46, 9, 10, 11 ...; check
// a clone's silkscreen). Wiring, host -> Sidecar: GPIO0 (TX) -> GPIO9,
// GPIO21 (RX) <- GPIO46, GPIO48 (BCLK) -> GPIO10. Why these:
// - The first rigs used 35/36/37, octal PSRAM pins on an N8R8 module: a
//   Sidecar held in reset held host GPIO21 (also the Atari refresh_ input)
//   low on miner6 board 6. These avoid the octal
//   and flash pins (26-37), USB (19/20), the console UART (43/44), the RGB
//   LED (38/48) and the strapping pins 0, 3 and 45.
// - Main app on this board by mistake: 9 and 10 are address inputs there
//   (never outputs; the bench FAKE_CLOCK setup only pulls them), so neither
//   end fights the host's BCLK or TX. Its GPIO46 drives host GPIO21, an input
//   in every image.
// - This app on the host board by mistake: TX lands on a pin only the ESP
//   drives (46 is MPD), and 9 and 10 become inputs on Atari address lines.
//   46 is the only usable output pin with two main-app inputs beside it.
// - 46 is a strapping pin, but with GPIO0 high (a flash boot) it only sets
//   ROM boot logging, and host GPIO21 at the far end at most pulls weakly.
// - BCLK on the outer pin, so only RX runs next to it.
const i2slink_config_t sidecarLinkConfig = {
    .clock = I2SLINK_CLOCK_FOLLOWER,
    .i2s_port = 0,         // I2S_NUM_0
    .bclk_gpio = 10,       // from the host's BCLK (GPIO48)
    .tx_gpio = 46,         // to the host's RX (GPIO21)
    .rx_gpio = 9,          // from the host's TX (GPIO0)
    .sync_timeout_us = 0,  // default: four wire cycles
    .loopback = false,
};

void sidecarAppBegin()
{
    mountSpiffs();
#ifndef CSIM
    mountSdCard();
#endif
    sio.setProcFsNodes(&procFsNodes);
    ESP_ERROR_CHECK(service.begin(sidecarLinkConfig));
    nextLog = esp_timer_get_time() + 1000000;
}

void sidecarAppPoll()
{
    // Other periodic Sidecar work goes here, alongside service().
    const bool handled = service.service();
    const int64_t now = esp_timer_get_time();
#ifndef CSIM
    if (handled) {
        lastCommandUs = now;
        gcIdle = false;
    } else {
        idleGc(now);
    }
#else
    (void)handled;
#endif
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
