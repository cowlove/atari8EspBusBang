// The Sidecar app: see sidecarApp.h.

#include "sidecarApp.h"

#include "sidecar.h"
#include "diskStitchProc.h"   // ProcFsFile (components/disk_image)
#include "sio.h"   // Sio, DiskImage (components/disk_image)
#include "gzipImage.h"    // hasGzipSuffix(), gzipImageInflate()
#include "gzipStream.h"   // the staged app.bin.gz's token

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_random.h"
#ifndef CSIM
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "mainReset.h"
#include "sidecarSampler.h"
#include "hal/usb_serial_jtag_ll.h"
#endif

#include <algorithm>
#ifndef CSIM
#include "psa/crypto.h"   // SHA-256 of a fetched core dump
#ifndef CSIM
#include "miniz.h"        // the ROM's deflate (tdefl), for the stored core
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif
#include <dirent.h>
#include <string>
#include <vector>
#endif
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

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

// Main's last boot report (BootStatus), from the handler; acted on in
// sidecarAppPoll().
SidecarBootStatusBody bootStatus;
bool bootStatusPending = false;
uint8_t bootReplyFlags = 0;   // the diagnostic flags the reply carried
bool bootCoreNew = false;     // the report showed a core dump not yet fetched
// Flags staged on the console (diag ...) for main's next boot report, one shot.
uint8_t nextBootFlags = 0;
uint32_t storedCoreToken(bool remove);

// Main's app against the one staged in /spiffs/main/app.bin.gz, decided in the
// BootStatus reply. Staging a new app.bin.gz is the deliberate act that updates
// main; the Sidecar then installs it by OTA (FetchFirmware) on main's next
// boot report, never with the serial flasher (a manual step: new boards,
// with the Atari disconnected).
enum class MainUpdate : uint8_t {
    None,       // nothing staged (or not an app image)
    Current,    // main runs the staged app
    Updating,   // replied FetchFirmware: main installs it and restarts
    Failed,     // an OTA to this image was already asked for and main still runs another
    NoSlot,     // main has no ota_1: update it by hand
    Deferred,   // a new core dump first; the update on a later boot
};
MainUpdate mainUpdate = MainUpdate::None;
uint32_t stagedApp = 0;      // the staged app's token, as of the last boot report
// The staged token an OTA was last asked for (this Sidecar boot only:
// restaging the spiffs restarts the Sidecar, which allows a new try).
uint32_t otaAskedFor = 0;

// The token main reports as appVersion for the staged app: the first 4
// bytes (little endian) of the ELF SHA-256 in its app description
// (esp_app_desc_t, the first thing in the image's first segment: offset
// 0x20, after the 24-byte image header and the 8-byte segment header;
// app_elf_sha256 at +0x90 in it). 0 if there's no app.bin.gz or it isn't an
// app image. Cheap: one 180-byte read.
uint32_t stagedAppToken(const char *path)
{
    constexpr long kDesc = 0x20, kSha = kDesc + 0x90;
    uint8_t b[kSha + 4];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    bool ok;
#ifndef CSIM
    if (hasGzipSuffix(path)) {
        // app.bin.gz: inflate just its first bytes (the first few KB of the
        // file are plenty for 180 inflated ones), then stop. The input on
        // the heap: this runs in the service task (a 4 KB stack: a 4 KB
        // buffer here overflowed it, a "Cache error" panic in the ROM's
        // inflater).
        constexpr size_t kIn = 4096;
        uint8_t *in = static_cast<uint8_t *>(malloc(kIn));
        const size_t n = in ? fread(in, 1, kIn, f) : 0;
        size_t got = 0;
        GzipStream peek([&](const uint8_t *data, size_t length) {
            const size_t take = std::min(length, sizeof(b) - got);
            memcpy(b + got, data, take);
            got += take;
            return got < sizeof(b);   // enough: stop
        });
        if (in) peek.push(in, n);
        free(in);
        ok = got == sizeof(b);
    } else
#endif
    {
        ok = fread(b, 1, sizeof(b), f) == sizeof(b);
    }
    fclose(f);
    auto le32 = [&](long at) {
        return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24);
    };
    if (!ok || b[0] != 0xe9 || le32(kDesc) != 0xabcd5432u) return 0;   // ESP image, app desc magic
    return le32(kSha);
}

// Main's console text (SerialOut), copied here by the handler and printed
// by sidecarAppPoll() after service() returns, so a slow console never
// holds up the response main waits for. A byte ring; full: the handler
// answers "dropped" and main counts it.
constexpr size_t kMainOutBytes = 8192;
uint8_t mainOut[kMainOutBytes];
size_t mainOutHead = 0, mainOutTail = 0;   // free-running; one task
uint32_t mainOutDropped = 0;

void printMainOut()
{
    if (mainOutTail == mainOutHead) {
        return;
    }
    while (mainOutTail != mainOutHead) {
        const size_t at = mainOutTail % kMainOutBytes;
        size_t n = mainOutHead - mainOutTail;
        if (n > kMainOutBytes - at) {
            n = kMainOutBytes - at;
        }
        fwrite(&mainOut[at], 1, n, stdout);
        mainOutTail += n;
    }
    fflush(stdout);
}

class AtariService : public SidecarService {
protected:
    void handleNop() override { ++counts.nop; }

    void handleSerialOut(const SerialOutCommand &command, SerialOutResponse &response) override
    {
        const size_t len = command.body.length;
        if (len > kMainOutBytes - (mainOutHead - mainOutTail)) {
            ++mainOutDropped;
            response.header.status = 1;   // dropped: no room
            return;
        }
        for (size_t i = 0; i < len; ++i) {
            mainOut[(mainOutHead + i) % kMainOutBytes] = command.data[i];
        }
        mainOutHead += len;
    }

    // Decide here, in the reply: main enters diagnostic mode at boot or not
    // at all. A core dump not yet fetched means main just crashed:
    // Diagnostic (then sidecarAppPoll() fetches it). Plus any flags staged
    // on the console for this boot.
    void handleBootStatus(const BootStatusCommand &command, BootStatusResponse &response) override
    {
        bootStatus = command.body;
        bootStatus.git[sizeof(bootStatus.git) - 1] = 0;
        bootCoreNew = (bootStatus.flags & static_cast<uint8_t>(SidecarBootFlag::CorePresent)) &&
                      bootStatus.coreVersion != storedCoreToken(false);
        uint8_t flags = nextBootFlags;
        nextBootFlags = 0;
        if (bootCoreNew) {
            flags |= static_cast<uint8_t>(SidecarDiagnosticFlag::Diagnostic);
        }
        // Main's app against the staged one (MainUpdate). A new core dump
        // goes first: it's fetched this boot, the update waits.
        stagedApp = stagedAppToken(kFirmwarePath);
        if (stagedApp == 0) {
            mainUpdate = MainUpdate::None;
        } else if (bootStatus.appVersion == stagedApp) {
            mainUpdate = MainUpdate::Current;
        } else if (bootCoreNew) {
            mainUpdate = MainUpdate::Deferred;
        } else if (!(bootStatus.flags & static_cast<uint8_t>(SidecarBootFlag::OtaSlot))) {
            mainUpdate = MainUpdate::NoSlot;
        } else if (otaAskedFor == stagedApp) {
            mainUpdate = MainUpdate::Failed;
        } else {
            mainUpdate = MainUpdate::Updating;
            otaAskedFor = stagedApp;
            flags |= static_cast<uint8_t>(SidecarDiagnosticFlag::FetchFirmware);
        }
        if (flags) {
            flags |= static_cast<uint8_t>(SidecarDiagnosticFlag::Diagnostic);
        }
        response.body.diagnostic = flags;
        response.body.capabilities = static_cast<uint8_t>(SidecarCapability::SerialOut);
        bootReplyFlags = flags;
        bootStatusPending = true;
    }

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
                printf("Sidecar: D%d: read sector %d failed: got %d of %d (errno %d)\n",
                       dcb.DUNIT, sector, (int)got, disk->specificSectorSize(sector), errno);
                response.body.DSTATS = 0;
                return;
            }
            succeed();
            break;
        case 0x50:
        case 0xd0:
        case 0x57:   // write sector (verify ignored)
            if (const int wrote = disk->write(command.data, sector); wrote < 0) {
                printf("Sidecar: D%d: write sector %d failed: %d (errno %d)\n",
                       dcb.DUNIT, sector, wrote, errno);
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

    // Link v2 reads of the Sidecar's objects (Test is built in): Firmware,
    // the main app image staged at kFirmwarePath (sidecar/flash_spiffs.sh),
    // for the main board to fetch (OTA).
    void handleReadVersion(const ReadVersionCommand &command, ReadVersionResponse &response) override
    {
        if (command.body.object != static_cast<uint8_t>(SidecarObject::Firmware)) {
            response.header.status = static_cast<uint8_t>(SidecarReadStatus::NoObject);
            return;
        }
        if (!firmwareRefresh()) {
            response.header.status = static_cast<uint8_t>(SidecarReadStatus::Unavailable);
            return;
        }
        response.body.size = firmware.size;
        response.body.version = firmware.crc;
        response.header.status = static_cast<uint8_t>(SidecarReadStatus::Ok);
    }

    void handleReadChunk(const ReadChunkCommand &command, ReadChunkResponse &response) override
    {
        if (command.body.object != static_cast<uint8_t>(SidecarObject::Firmware)) {
            response.header.status = static_cast<uint8_t>(SidecarReadStatus::NoObject);
            return;
        }
        if (!firmwareRefresh()) {
            response.header.status = static_cast<uint8_t>(SidecarReadStatus::Unavailable);
            return;
        }
        const uint32_t offset = command.body.offset;
        uint32_t length = offset < firmware.size ? firmware.size - offset : 0;
        length = std::min<uint32_t>(length, sizeof(response.data));
        if (length && (!firmware.file || fseek(firmware.file, offset, SEEK_SET) != 0 ||
                       fread(response.data, 1, length, firmware.file) != length)) {
            response.header.status = static_cast<uint8_t>(SidecarReadStatus::IoError);
            return;
        }
        response.body.length = static_cast<uint16_t>(length);
        response.header.status = static_cast<uint8_t>(SidecarReadStatus::Ok);
    }

public:
    // The staged main firmware: size and CRC32 (the version token), and an
    // open file for the chunk reads (the protocol is stateless; keeping the
    // file open is this side's business). Recomputed when the size changes
    // (a restaged image of another size), else kept from the first look.
    // gzipped (sidecar/flash_spiffs.sh): served as is; main inflates it as
    // it writes its OTA slot (SidecarHost::installFirmware()).
    static constexpr const char *kFirmwarePath = "/spiffs/main/app.bin.gz";
    struct {
        bool known = false;
        uint32_t size = 0, crc = 0;
        FILE *file = nullptr;
    } firmware;

    bool firmwareRefresh()
    {
        struct stat st;
        if (stat(kFirmwarePath, &st) != 0) {
            if (firmware.file) fclose(firmware.file);
            firmware = {};
            return false;
        }
        if (firmware.known && static_cast<uint32_t>(st.st_size) == firmware.size && firmware.file)
            return true;
        if (firmware.file) fclose(firmware.file);
        firmware = {};
        firmware.file = fopen(kFirmwarePath, "rb");
        if (!firmware.file) return false;
        uint8_t block[1024];
        uint32_t crc = 0;
        size_t n;
        while ((n = fread(block, 1, sizeof(block), firmware.file)) > 0)
            crc = sidecarCrc32(crc, block, n);
        firmware.size = static_cast<uint32_t>(st.st_size);
        firmware.crc = crc;
        firmware.known = true;
        printf("Sidecar: Firmware object: %s, %u bytes, crc %08x\n", kFirmwarePath,
               (unsigned)firmware.size, (unsigned)firmware.crc);
        return true;
    }
};

AtariService service;

// Mount the Sidecar's SPIFFS (the host's spiffs.bin) at /spiffs, where sio
// looks for the diskSpecs the host sends in Mount commands (Sio::resolve()).
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
}

#ifndef CSIM
// Optional microSD card, 4-bit SDMMC through the GPIO matrix, on six
// adjacent pins (DevKitC-1 J3-4..9) in the socket's pin order; see
// docs/sidecar_wiring.pdf. A card that doesn't answer in SD mode is tried
// again in SPI mode on four of the same pins (CS = DAT3, MOSI = CMD,
// MISO = DAT0, CLK), for the common SPI-only breakouts, whose one-way level
// shifters can't carry SD mode's bidirectional CMD and DAT lines. The pins avoid the strapping, USB, UART0,
// flash/PSRAM, RGB LED, link and bus-profiler pins. CMD and DAT0-3 need
// 10 k external pull-ups; the internal ones are enabled too but are too
// weak alone.
constexpr gpio_num_t kSdClk = GPIO_NUM_41, kSdCmd = GPIO_NUM_42, kSdD0 = GPIO_NUM_40,
                     kSdD1 = GPIO_NUM_39, kSdD2 = GPIO_NUM_1, kSdD3 = GPIO_NUM_2;
sdmmc_card_t *sdCard = nullptr;
const char *sdMode = "";

// Never reformat a user's card on its own: only sdformat (sdFormat())
// passes format, after zeroing the partition table.
esp_vfs_fat_sdmmc_mount_config_t sdMountConfig(bool format)
{
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = format;
    mount.max_files = 5;
    mount.allocation_unit_size = 32 * 1024;   // a format's cluster size (FAT32)
    return mount;
}

esp_err_t trySdMount(int maxFreqKhz, bool format = false, int width = 4)
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
    slot.width = width;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mount = sdMountConfig(format);
    return esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mount, &sdCard);
}

// SPI mode: the card enters it when it sees CMD0 with CS (DAT3) low, even
// after the SD-mode attempt, and stays in it until power-cycled. SPI2 is
// otherwise unused on the Sidecar.
constexpr spi_host_device_t kSdSpiHost = SPI2_HOST;

esp_err_t trySdSpiMount(int maxFreqKhz, bool format = false)
{
    // Undo the SD-mode attempt's pin routing (DAT3 becomes a plain GPIO CS).
    for (gpio_num_t pin : {kSdClk, kSdCmd, kSdD0, kSdD1, kSdD2, kSdD3})
        gpio_reset_pin(pin);
    spi_bus_config_t bus = {};
    bus.mosi_io_num = kSdCmd;
    bus.miso_io_num = kSdD0;
    bus.sclk_io_num = kSdClk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t err = spi_bus_initialize(kSdSpiHost, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK)
        return err;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = kSdSpiHost;
    host.max_freq_khz = maxFreqKhz;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = kSdD3;
    slot.host_id = kSdSpiHost;
    esp_vfs_fat_sdmmc_mount_config_t mount = sdMountConfig(format);
    err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &mount, &sdCard);
    if (err != ESP_OK)
        spi_bus_free(kSdSpiHost);
    return err;
}

// Mount the card's FAT volume at /sd if there is one. No card, a card that
// won't initialize or one without a FAT volume is fine: the Sidecar just
// runs without it.
void mountSdCard(bool format = false)
{
    const int64_t start = esp_timer_get_time();
    sdMode = "4-bit SD";
    esp_err_t err = trySdMount(SDMMC_FREQ_HIGHSPEED, format);
    // No card shows up as a timeout; anything else may be the 40 MHz clock
    // (long wires, a slow card), so try once more at the default 20 MHz.
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
        err = trySdMount(SDMMC_FREQ_DEFAULT, format);
    // The card answers in SD mode but 4-bit fails (a bad DAT1-DAT3 line:
    // the first 4-bit read, the SSR, fails its CRC): 1-bit SD mode needs
    // only CMD, CLK and DAT0, and stays faster than SPI.
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        sdMode = "1-bit SD";
        err = trySdMount(SDMMC_FREQ_HIGHSPEED, format, 1);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
            err = trySdMount(SDMMC_FREQ_DEFAULT, format, 1);
    }
    // Then SPI mode (an SPI-only breakout times out in SD mode too), again
    // slower if 20 MHz isn't clean.
    if (err != ESP_OK) {
        sdMode = "SPI";
        err = trySdSpiMount(SDMMC_FREQ_DEFAULT, format);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
            err = trySdSpiMount(4000, format);   // kHz
    }
    const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        sdCard = nullptr;
        printf("Sidecar: no SD card (%s, %d ms); continuing without it\n",
               esp_err_to_name(err), ms);
        return;
    }
    const uint64_t mb = uint64_t(sdCard->csd.capacity) * sdCard->csd.sector_size >> 20;
    printf("Sidecar: SD card '%s' mounted at /sd: %llu MB, %d kHz, %s (%d ms)\n",
           sdCard->cid.name, static_cast<unsigned long long>(mb), sdCard->real_freq_khz,
           sdMode, ms);
}
#endif

// The card without a filesystem: SD mode, else SPI (as mountSdCard()), at
// the default clock. On success *card is initialized; sdRawClose() undoes it.
esp_err_t sdRawOpen(sdmmc_card_t *card, bool *spi, sdspi_dev_handle_t *spiDev)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = kSdClk;
    slot.cmd = kSdCmd;
    slot.d0 = kSdD0;
    slot.d1 = kSdD1;
    slot.d2 = kSdD2;
    slot.d3 = kSdD3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    *spi = false;
    esp_err_t err = sdmmc_host_init();
    if (err == ESP_OK)
        err = sdmmc_host_init_slot(host.slot, &slot);
    if (err == ESP_OK)
        err = sdmmc_card_init(&host, card);
    if (err == ESP_OK)
        return ESP_OK;
    sdmmc_host_deinit();

    *spi = true;
    for (gpio_num_t pin : {kSdClk, kSdCmd, kSdD0, kSdD1, kSdD2, kSdD3})
        gpio_reset_pin(pin);
    spi_bus_config_t bus = {};
    bus.mosi_io_num = kSdCmd;
    bus.miso_io_num = kSdD0;
    bus.sclk_io_num = kSdClk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    err = spi_bus_initialize(kSdSpiHost, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK)
        return err;
    sdmmc_host_t spiHost = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = kSdD3;
    dev.host_id = kSdSpiHost;
    err = sdspi_host_init();
    if (err == ESP_OK)
        err = sdspi_host_init_device(&dev, spiDev);
    spiHost.slot = *spiDev;
    if (err == ESP_OK)
        err = sdmmc_card_init(&spiHost, card);
    if (err != ESP_OK) {
        sdspi_host_deinit();
        spi_bus_free(kSdSpiHost);
    }
    return err;
}

void sdRawClose(bool spi, sdspi_dev_handle_t spiDev)
{
    if (spi) {
        sdspi_host_remove_device(spiDev);
        sdspi_host_deinit();
        spi_bus_free(kSdSpiHost);
    } else {
        sdmmc_host_deinit();
    }
}

// Console "sdformat yes": erase the card's partition table and give it one
// MBR partition with FAT32 (32 KB clusters), then mount it at /sd. MBR
// because IDF's FatFs reads no GPT (FF_LBA64 off): a card partitioned GPT
// (a protective MBR, type 0xEE) mounts as "no filesystem". Sector 0 is
// zeroed, and a GPT's primary and backup headers (sector 1, the last
// sector), so nothing reads the card as GPT afterwards; the mount then
// finds no filesystem and IDF partitions and formats it (format_if_mount_
// failed, only here). Whatever is open on /sd must be closed first.
void sdFormat()
{
    if (sdCard) {
        printf("Sidecar: sdformat: unmounting /sd\n");
        const bool wasSpi = strcmp(sdMode, "SPI") == 0;
        esp_vfs_fat_sdcard_unmount("/sd", sdCard);
        if (wasSpi)
            spi_bus_free(kSdSpiHost);
        sdCard = nullptr;
    }
    sdmmc_card_t card = {};
    bool spi = false;
    sdspi_dev_handle_t spiDev = 0;
    esp_err_t err = sdRawOpen(&card, &spi, &spiDev);
    if (err != ESP_OK) {
        printf("Sidecar: sdformat: no card (%s)\n", esp_err_to_name(err));
        return;
    }
    const uint32_t sectors = card.csd.capacity;
    uint8_t *buf = static_cast<uint8_t *>(heap_caps_calloc(1, 512, MALLOC_CAP_DMA));
    err = sdmmc_read_sectors(&card, buf, 0, 1);
    const bool gpt = err == ESP_OK && buf[510] == 0x55 && buf[511] == 0xaa && buf[0x1c2] == 0xee;
    memset(buf, 0, 512);
    if (err == ESP_OK)
        err = sdmmc_write_sectors(&card, buf, 0, 1);
    if (err == ESP_OK && gpt)
        err = sdmmc_write_sectors(&card, buf, 1, 1);
    if (err == ESP_OK && gpt)
        err = sdmmc_write_sectors(&card, buf, sectors - 1, 1);
    heap_caps_free(buf);
    sdRawClose(spi, spiDev);
    printf("Sidecar: sdformat: %u MB card, %s mode; partition table %s%s\n",
           (unsigned)(uint64_t(sectors) * card.csd.sector_size >> 20), spi ? "SPI" : "SD",
           err == ESP_OK ? "erased" : "erase failed: ", err == ESP_OK ? (gpt ? " (was GPT)" : "") : esp_err_to_name(err));
    if (err != ESP_OK)
        return;
    printf("Sidecar: sdformat: partitioning and formatting FAT32 (can take a minute)...\n");
    mountSdCard(true);
}

int64_t nextLog = 0;
Counts lastCounts = {};
i2slink_stats_t lastStats = {};

#ifndef CSIM
// Console commands, typed on the Sidecar's USB serial console (a line each):
//   reset        reset the main board into its firmware (unchecked: with an
//                Atari attached too); its boot report then decides an OTA
//                update from /spiffs/main/app.bin.gz or a core fetch
//   report       print the bus sampler's histograms now
//   sdformat yes erase the SD card: one MBR partition, FAT32, mounted at /sd
//   cp <src> <dst>  copy a file, e.g. cp /spiffs/d1.atr.gz /sd/d1.atr.gz
//   gunzip <src> <dst>  inflate a gzip file, e.g. gunzip /sd/d1.atr.gz /sd/d1.atr
//   ls [dir]     list a directory (default /sd)
// No serial flashing here: that's sidecar_serial_flasher, a separate app
// flashed by hand, with the Atari disconnected.
char consoleLine[128];
size_t consoleLen = 0;


// The console's input, read straight from the USB-Serial-JTAG receive FIFO:
// without its driver, ESP-IDF's console is output-only, and the driver
// would make printf block whenever no host is listening.
void pollConsole()
{
    uint8_t buf[16];
    int n;
    while ((n = usb_serial_jtag_ll_rxfifo_data_available() ? usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf)) : 0) > 0)
    for (int i = 0; i < n; ++i) {
        const char c = static_cast<char>(buf[i]);
        if (c == '\r' || c == '\n') {
            consoleLine[consoleLen] = 0;
            if (consoleLen) sidecarAppCommand(consoleLine);
            consoleLen = 0;
        } else if (consoleLen + 1 < sizeof(consoleLine)) {
            consoleLine[consoleLen++] = c;
        }
    }
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

namespace {
const char *objectName(SidecarObject object)
{
    switch (object) {
    case SidecarObject::Firmware: return "Firmware";
    case SidecarObject::Core: return "Core";
    case SidecarObject::Test: return "Test";
    }
    return "?";
}

// Fetch an object from the main board (link v2) and report.
void fetchReport(SidecarObject object)
{
    const int64_t start = esp_timer_get_time();
    uint32_t wrong = 0;
    const SidecarFetchResult r = sidecarFetch(service, object,
        [&](uint32_t offset, const uint8_t *data, uint16_t length) {
            if (object == SidecarObject::Test) {
                for (uint16_t i = 0; i < length; ++i)
                    wrong += data[i] != sidecarTestObjectByte(offset + i);
            }
            return true;
        });
    const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    printf("Sidecar fetch %s from main: link %d, status %d, %u of %u bytes, crc %08x, version "
           "%08x, %s%s, %d ms (%.1f kB/s), preempted %u\n", objectName(object), (int)r.link,
           (int)r.status, (unsigned)r.received, (unsigned)r.size, (unsigned)r.crc,
           (unsigned)r.version,
           r.consistent && (r.crcMatches || object == SidecarObject::Core) ? "OK" : "INCONSISTENT",
           object == SidecarObject::Test ? (wrong ? ", PATTERN WRONG" : ", pattern ok") : "", ms,
           ms ? r.received / 1.024 / ms : 0.0, (unsigned)service.preempted());
}
}  // namespace

namespace {
// Main's last fetched core dump, gzipped (an ESP-IDF core dump image inside:
// boot.sh -sc pulls it out of a dump of this spiffs, tools/spiffs_extract.py,
// gunzips it and decodes it against main's ELF). One file, the latest, named
// by its token, so "already fetched" is a name check: core/main-<token>.core.gz.
// A new crash replaces it. Another storage (an SD card) is another prefix.
constexpr const char *kCoreDir = "/spiffs";
constexpr const char *kCorePrefix = "core/main-";      // names as SPIFFS lists them
constexpr const char *kCoreSuffix = ".core.gz";
constexpr const char *kMainCoreTmpPath = "/spiffs/core/main.core.gz.tmp";
constexpr size_t kCoreWriteBuf = 32768;   // the writer's stdio buffer (PSRAM, kept)

std::string corePath(uint32_t token)
{
    char name[64];
    snprintf(name, sizeof(name), "%s/%s%08x%s", kCoreDir, kCorePrefix, (unsigned)token, kCoreSuffix);
    return name;
}

// The stored core's token (from its name: the first 4 bytes, little endian,
// of the SHA-256 IDF keeps in a core dump's last 32 bytes, the same token
// main's boot report carries as coreVersion). A core main reports with the
// same token is one already fetched. 0: none. remove: delete every stored
// one instead (before a new fetch).
uint32_t storedCoreToken(bool remove = false)
{
    uint32_t v = 0;
    DIR *dir = opendir(kCoreDir);
    if (!dir) return 0;
    const size_t pre = strlen(kCorePrefix), suf = strlen(kCoreSuffix);
    std::vector<std::string> found;
    while (const dirent *e = readdir(dir)) {
        const size_t n = strlen(e->d_name);
        if (n == pre + 8 + suf && strncmp(e->d_name, kCorePrefix, pre) == 0 &&
            strcmp(e->d_name + pre + 8, kCoreSuffix) == 0) {
            found.push_back(e->d_name);
        }
    }
    closedir(dir);
    for (const std::string &name : found) {
        if (remove) {
            ::remove((std::string(kCoreDir) + "/" + name).c_str());
        } else {
            v = static_cast<uint32_t>(strtoul(name.substr(pre, 8).c_str(), nullptr, 16));
        }
    }
    return v;
}

#ifndef CSIM
// The core writer: a task on core 0 that gzips the fetched bytes and writes
// them, while the fetch (core 1) carries on. Chunks go through two queues of
// buffer pointers (free, full); a full writer holds the fetch back. Deflate
// is the ROM's miniz (tdefl: greedy, 16 probes: fast, and core dumps are
// mostly zeros), its state in PSRAM. The gzip stream: a 10-byte header, raw
// deflate, crc32 and length.
struct CoreChunk {
    uint16_t length;   // kChunkEnd: the end of the stream
    uint8_t data[ReadChunkResponse::kDataBytes];
};
constexpr uint16_t kChunkEnd = 0xffff;
constexpr int kCoreChunks = 64;
struct CoreWriter {
    QueueHandle_t freeQ = nullptr, fullQ = nullptr;
    SemaphoreHandle_t done = nullptr;
    CoreChunk *chunks = nullptr;
    tdefl_compressor *deflate = nullptr;
    char *buf = nullptr;
    // One job's state, set by fetchCore() before the first chunk.
    FILE *out = nullptr;
    bool ok = false;
    uint32_t crc = 0, inBytes = 0, outBytes = 0;
    int64_t deflateUs = 0, writeUs = 0;
} writer;

mz_bool corePut(const void *data, int length, void *)
{
    const int64_t t0 = esp_timer_get_time();
    const bool ok = fwrite(data, 1, length, writer.out) == static_cast<size_t>(length);
    writer.writeUs += esp_timer_get_time() - t0;
    writer.outBytes += length;
    writer.ok = writer.ok && ok;
    return ok;
}

void coreWriterTask(void *)
{
    for (;;) {
        CoreChunk *c;
        xQueueReceive(writer.fullQ, &c, portMAX_DELAY);
        if (c->length == kChunkEnd) {
            const int64_t t0 = esp_timer_get_time();
            const int64_t w0 = writer.writeUs;
            writer.ok = tdefl_compress_buffer(writer.deflate, nullptr, 0, TDEFL_FINISH) ==
                            TDEFL_STATUS_DONE && writer.ok;
            writer.deflateUs += esp_timer_get_time() - t0 - (writer.writeUs - w0);
            uint8_t trailer[8];
            memcpy(trailer, &writer.crc, 4);
            memcpy(trailer + 4, &writer.inBytes, 4);
            corePut(trailer, sizeof(trailer), nullptr);
            xQueueSend(writer.freeQ, &c, portMAX_DELAY);
            xSemaphoreGive(writer.done);
            continue;
        }
        if (writer.ok) {
            writer.crc = static_cast<uint32_t>(mz_crc32(writer.crc, c->data, c->length));
            writer.inBytes += c->length;
            const int64_t t0 = esp_timer_get_time();
            const int64_t w0 = writer.writeUs;
            writer.ok = tdefl_compress_buffer(writer.deflate, c->data, c->length, TDEFL_NO_FLUSH) ==
                        TDEFL_STATUS_OKAY && writer.ok;
            writer.deflateUs += esp_timer_get_time() - t0 - (writer.writeUs - w0);
        }
        xQueueSend(writer.freeQ, &c, portMAX_DELAY);
    }
}

bool coreWriterInit()
{
    if (writer.fullQ) return true;
    writer.chunks = static_cast<CoreChunk *>(malloc(sizeof(CoreChunk) * kCoreChunks));
    writer.deflate = static_cast<tdefl_compressor *>(
        heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    writer.buf = static_cast<char *>(malloc(kCoreWriteBuf));
    writer.freeQ = xQueueCreate(kCoreChunks, sizeof(CoreChunk *));
    writer.fullQ = xQueueCreate(kCoreChunks, sizeof(CoreChunk *));
    writer.done = xSemaphoreCreateBinary();
    if (!writer.chunks || !writer.deflate || !writer.buf || !writer.freeQ || !writer.fullQ ||
        !writer.done) {
        printf("Sidecar: core writer: out of memory\n");
        return false;
    }
    for (int k = 0; k < kCoreChunks; ++k) {
        CoreChunk *c = &writer.chunks[k];
        xQueueSend(writer.freeQ, &c, 0);
    }
    // Core 0, above the bus sampler (which sleeps while main is in
    // diagnostic mode, as it is for any core fetch); the service loop has
    // core 1 to itself.
    // 16 KB of stack: the ROM's deflate (Huffman table building) plus the
    // SPIFFS write path under it overflowed 4 KB.
    xTaskCreatePinnedToCore(coreWriterTask, "corewriter", 16384, nullptr, tskIDLE_PRIORITY + 3,
                            nullptr, 0);
    return true;
}

#endif  // CSIM

// Fetch main's core dump (main must be in diagnostic mode: it only serves
// Core there) and check it: all of it, the same token before and after, and
// the SHA-256 IDF stored in its last 32 bytes (over everything before them)
// recomputed here on the raw bytes. Store: gzipped by the writer task to a
// temporary file, renamed to core/main-<token>.core.gz only once it all
// checks out.
// sink: what's done with the bytes. Store (the default): hash and write;
// Hash, Null: for measuring (console "fetch core hash|null"), the files
// untouched. The timings split the fetch: the hash; the writer's deflate
// and writes (on core 0, overlapping the fetch); the wait for the writer
// (queue full, or its finish); and the rest, which is the link round trips
// and main's flash reads (the Test object, from main's RAM, is the link
// alone).
enum class CoreSink : uint8_t { Store, Hash, Null };
struct CoreFetch {
    SidecarFetchResult r;
    bool shaOk = false;
    bool stored = false;
    std::string path;   // stored: where
    int ms = 0;
    int64_t hashUs = 0, waitUs = 0, deflateUs = 0, writeUs = 0;
    uint32_t chunks = 0, gzBytes = 0;
};
CoreFetch fetchCore(CoreSink sink = CoreSink::Store)
{
    CoreFetch f;
    const int64_t start = esp_timer_get_time();
#ifndef CSIM
    const bool hash = sink != CoreSink::Null;
    bool store = sink == CoreSink::Store && coreWriterInit();
    if (store) {
        // The old one goes first: two full dumps needn't fit at once, and
        // it's been bundled, or is stale.
        remove(kMainCoreTmpPath);
        storedCoreToken(true);
        writer.out = fopen(kMainCoreTmpPath, "wb");
        if (!writer.out) {
            printf("Sidecar: can't create %s (errno %d); fetching without storing\n",
                   kMainCoreTmpPath, errno);
            store = false;
        } else {
            setvbuf(writer.out, writer.buf, _IOFBF, kCoreWriteBuf);
            writer.ok = true;
            writer.crc = writer.inBytes = writer.outBytes = 0;
            writer.deflateUs = writer.writeUs = 0;
            tdefl_init(writer.deflate, corePut, nullptr, 16 | TDEFL_GREEDY_PARSING_FLAG);
            static const uint8_t header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
            corePut(header, sizeof(header), nullptr);
        }
    }
    psa_crypto_init();
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&sha, PSA_ALG_SHA_256);
    // The last 32 bytes seen so far: at the end, the stored SHA-256. Bytes
    // that fall out of it feed the hash (the sink doesn't know the size).
    uint8_t tail[32];
    uint32_t tailLen = 0;
    uint32_t total = 0;
#endif
    f.r = sidecarFetch(service, SidecarObject::Core,
        [&](uint32_t offset, const uint8_t *data, uint16_t length) {
#ifndef CSIM
            ++f.chunks;
            total = offset + length;
            if (hash) {
                const int64_t t0 = esp_timer_get_time();
                uint8_t joined[sizeof(tail) + ReadChunkResponse::kDataBytes];
                memcpy(joined, tail, tailLen);
                memcpy(joined + tailLen, data, length);
                const uint32_t n = tailLen + length;
                const uint32_t keep = std::min<uint32_t>(n, sizeof(tail));
                if (n > keep) psa_hash_update(&sha, joined, n - keep);
                memcpy(tail, joined + n - keep, keep);
                tailLen = keep;
                f.hashUs += esp_timer_get_time() - t0;
            }
            if (store) {
                const int64_t t0 = esp_timer_get_time();
                CoreChunk *c;
                xQueueReceive(writer.freeQ, &c, portMAX_DELAY);   // the writer's backpressure
                f.waitUs += esp_timer_get_time() - t0;
                c->length = length;
                memcpy(c->data, data, length);
                xQueueSend(writer.fullQ, &c, portMAX_DELAY);
            }
#else
            (void)offset; (void)data; (void)length;
#endif
            return true;
        });
#ifndef CSIM
    uint8_t computed[32];
    size_t computedLen = 0;
    f.shaOk = psa_hash_finish(&sha, computed, sizeof(computed), &computedLen) == PSA_SUCCESS &&
              hash && total > sizeof(tail) && tailLen == sizeof(tail) &&
              memcmp(computed, tail, sizeof(tail)) == 0;
    if (store) {
        const int64_t t0 = esp_timer_get_time();
        CoreChunk *c;
        xQueueReceive(writer.freeQ, &c, portMAX_DELAY);
        c->length = kChunkEnd;
        xQueueSend(writer.fullQ, &c, portMAX_DELAY);
        xSemaphoreTake(writer.done, portMAX_DELAY);
        const int64_t c0 = esp_timer_get_time();
        const bool closed = fclose(writer.out) == 0;
        writer.writeUs += esp_timer_get_time() - c0;
        writer.out = nullptr;
        f.waitUs += esp_timer_get_time() - t0;
        f.deflateUs = writer.deflateUs;
        f.writeUs = writer.writeUs;
        f.gzBytes = writer.outBytes;
        const bool fetched = f.r.link == SidecarResult::Ok && f.r.status == SidecarReadStatus::Ok &&
                             f.r.consistent && f.shaOk;
        if (writer.ok && closed && fetched) {
            f.path = corePath(f.r.version);
            f.stored = rename(kMainCoreTmpPath, f.path.c_str()) == 0;
        }
        if (!f.stored) {
            remove(kMainCoreTmpPath);
        }
    }
#endif
    f.ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    return f;
}

// Where a core fetch's time went (see CoreFetch).
void printCoreFetchTiming(const char *what, const CoreFetch &f)
{
    const int hashMs = static_cast<int>(f.hashUs / 1000), waitMs = static_cast<int>(f.waitUs / 1000);
    const int rest = f.ms - hashMs - waitMs;
    printf("Sidecar: %s timing: %u bytes in %u chunks, %d ms (%.1f kB/s): hash %d ms, waiting for "
           "the writer %d ms, link + main %d ms (%.2f ms/chunk)", what, (unsigned)f.r.received,
           (unsigned)f.chunks, f.ms, f.ms ? f.r.received / 1.024 / f.ms : 0.0, hashMs, waitMs, rest,
           f.chunks ? double(rest) / f.chunks : 0.0);
    if (f.gzBytes) {
        printf("; writer (core 0): deflate %d ms, write %d ms, gzip %u bytes (%.1fx)",
               static_cast<int>(f.deflateUs / 1000), static_cast<int>(f.writeUs / 1000),
               (unsigned)f.gzBytes, f.gzBytes ? double(f.r.received) / f.gzBytes : 0.0);
    }
    printf("\n");
}

// Console "corehold on": a new core dump in main's boot report puts main in
// diagnostic mode as usual, but isn't fetched or erased, so "fetch core
// [hash|null]" can fetch it as often as wanted. Off at boot.
bool coreHold = false;

// The Sidecar after a core fetch: it reports and stops serving (sidecarAppPoll()
// keeps only the console), and main stays in diagnostic mode, until someone
// resets them. A runner (boot.sh) sees the crash and bundles the core dump.
bool halted = false;

// After main's boot report (the reply already went out, with
// bootReplyFlags). A new core dump: main is entering diagnostic mode (the
// bus never attached; the Atari, which ran on a garbage bus through the
// panic and reboot, is now simply unserved; the planned hardware holds its
// reset too): fetch the core, then halt. FetchFirmware: main pulls the
// Firmware object itself; this side just serves it.
void handleBootReport()
{
    const SidecarBootStatusBody &s = bootStatus;
    printf("Sidecar: main booted: git %s, reset reason %d, flags %02x (%s%s%s), core %u bytes "
           "%08x, app %08x; replied diagnostic %02x\n",
           s.git, s.resetReason, s.flags,
           s.flags & static_cast<uint8_t>(SidecarBootFlag::CorePartition) ? "core partition" : "",
           s.flags & static_cast<uint8_t>(SidecarBootFlag::CorePresent) ? ", core present" : "",
           s.flags & static_cast<uint8_t>(SidecarBootFlag::OtaSlot) ? ", ota slot" : "",
           (unsigned)s.coreSize, (unsigned)s.coreVersion, (unsigned)s.appVersion, bootReplyFlags);
    // The update decision (boot.sh -sc waits for these lines).
    switch (mainUpdate) {
    case MainUpdate::None:
        printf("Sidecar: main app %08x; no app staged in %s\n", (unsigned)s.appVersion,
               AtariService::kFirmwarePath);
        break;
    case MainUpdate::Current:
        printf("Sidecar: main app %08x is the staged app: current\n", (unsigned)s.appVersion);
        break;
    case MainUpdate::Updating:
        printf("Sidecar: main app %08x, staged %08x: updating main by OTA\n",
               (unsigned)s.appVersion, (unsigned)stagedApp);
        break;
    case MainUpdate::Failed:
        printf("Sidecar: MAIN_UPDATE_FAILED: main app %08x, staged %08x: the OTA asked for "
               "didn't take; not retrying (restage, or restart the Sidecar); main boots its app\n",
               (unsigned)s.appVersion, (unsigned)stagedApp);
        break;
    case MainUpdate::NoSlot:
        printf("Sidecar: MAIN_UPDATE_FAILED: main app %08x, staged %08x: main has no OTA slot "
               "(ota_1); update it by hand\n", (unsigned)s.appVersion, (unsigned)stagedApp);
        break;
    case MainUpdate::Deferred:
        printf("Sidecar: main app %08x, staged %08x: a new core dump first; the update on a "
               "later boot\n", (unsigned)s.appVersion, (unsigned)stagedApp);
        break;
    }
    if (bootReplyFlags & static_cast<uint8_t>(SidecarDiagnosticFlag::FetchFirmware)) {
        printf("Sidecar: main will fetch and install Firmware (OTA); serving it\n");
        return;   // a core fetch would collide with main's fetch; it can wait for the next boot
    }
    if (!bootCoreNew) {
        return;
    }
    if (coreHold) {
        printf("Sidecar: new core dump %08x (%u bytes) held (corehold on): main is in diagnostic "
               "mode; \"fetch core [hash|null]\" fetches it\n", (unsigned)s.coreVersion,
               (unsigned)s.coreSize);
        return;
    }
    printf("Sidecar: new core dump %08x: main just crashed; it's in diagnostic mode; fetching\n",
           (unsigned)s.coreVersion);
    CoreFetch f = fetchCore();
    // Read whole and consistent, but its SHA-256 doesn't check out: once
    // more, in case the transfer garbled it; the same again means the dump
    // itself is bad (or not a dump: stale flash in a coredump partition that
    // moved). Then it's erased like a good one, or every boot would stop
    // here.
    const auto readWhole = [&](const CoreFetch &c) {
        return c.r.link == SidecarResult::Ok && c.r.status == SidecarReadStatus::Ok &&
               c.r.consistent && c.r.version == s.coreVersion;
    };
    bool corrupt = false;
    if (readWhole(f) && !f.shaOk) {
        printf("Sidecar: core dump %08x: SHA-256 doesn't check out; fetching it again\n",
               (unsigned)s.coreVersion);
        f = fetchCore();
        corrupt = readWhole(f) && !f.shaOk;
    }
    const bool ok = readWhole(f) && f.shaOk;
    printCoreFetchTiming("core fetch", f);
    printf("Sidecar: core dump %08x: %u of %u bytes, %s, sha %s, %s, %d ms (%.1f kB/s)\n",
           (unsigned)f.r.version, (unsigned)f.r.received, (unsigned)f.r.size,
           f.r.consistent ? "consistent" : "INCONSISTENT", f.shaOk ? "ok" : "BAD",
           f.stored ? f.path.c_str() : "NOT STORED", f.ms,
           f.ms ? f.r.received / 1.024 / f.ms : 0.0);
    // Stored and verified: have main erase it, so its next boot report shows
    // no core dump (one in main's flash is one not yet fetched). Risk,
    // accepted for now (AGENTS.md TODO): the copy here is then the only one
    // until boot.sh has pulled it out of this spiffs and bundled it.
    if ((ok && f.stored) || corrupt) {
        const int64_t t0 = esp_timer_get_time();
        SidecarEraseStatus st = SidecarEraseStatus::Unavailable;
        const SidecarResult r = service.eraseCore(s.coreVersion, &st, 30000000);
        printf("Sidecar: EraseCore %08x: link %d, status %d (%s), %d ms\n", (unsigned)s.coreVersion,
               (int)r, (int)st,
               r != SidecarResult::Ok ? "no answer" :
               st == SidecarEraseStatus::Erased ? "erased in main's flash" :
               st == SidecarEraseStatus::OtherCore ? "main holds another core dump: left alone" :
               st == SidecarEraseStatus::Unavailable ? "main has none, or isn't in diagnostic mode" :
               "erase failed",
               static_cast<int>((esp_timer_get_time() - t0) / 1000));
    }
    printf("SIDECAR HALT: main crashed (reset reason %d, git %s); core dump %08x %s; main left "
           "in diagnostic mode; reset both to continue\n",
           s.resetReason, s.git, (unsigned)s.coreVersion,
           ok && f.stored ? "fetched, verified and stored" :
           corrupt        ? "CORRUPT (its SHA-256 fails twice), erased" : "FETCH FAILED");
    // For the runner (boot.sh -sc): the core to pull out of this spiffs (a
    // dump of it, tools/spiffs_extract.py), then the end of the run.
    if (ok && f.stored) {
        printf("COREFILE_AVAILABLE main %s %u %08x\n", f.path.c_str(), (unsigned)f.r.received,
               (unsigned)s.coreVersion);
    } else if (corrupt) {
        printf("COREFILE_CORRUPT main %08x %u\n", (unsigned)s.coreVersion, (unsigned)f.r.received);
    } else {
        printf("COREFILE_FETCH_FAILED main %08x\n", (unsigned)s.coreVersion);
    }
    printf("DONE (Sidecar: main crashed)\n");
    fflush(stdout);
    halted = true;
}
}  // namespace

namespace {
// Lock-break stress test (bench): break this side's receive lock tens of
// times a second and time the resync, through one read path:
//   peek  i2slink_peek() in a loop (the fail-fast one-look read)
//   read  i2slink_read(..., 0) in a loop
//   hint  i2slink_read(..., 0) only after a true i2slink_hint(): the
//         service loop's path
// Breaks alternate: a stale lock (still marked locked, at a wrong bit
// phase/lane/marker, as after a peer's clock restart) and a dropped one.
// The wire is untouched, so the right lock is the one before the break: a
// recovery is the link locked there again; a false lock is any NEW frame
// returned with the lock anywhere else. Between breaks (a 50 ms period) the
// link is serviced normally, so main's SIO through the Sidecar keeps going;
// the frame on the wire is offered again first (i2slink_test_redeliver()),
// since the stress loop's own reads bypass the protocol.
void lockStress(const char *mode, int seconds)
{
    i2slink_t *link = service.link();
    const int path = strcmp(mode, "peek") == 0 ? 0 : strcmp(mode, "read") == 0 ? 1 : 2;
    static uint8_t buf[I2SLINK_PAYLOAD_BYTES];
    uint8_t gp = 0, gl = 0, p, l;
    uint16_t gm = 0, m;
    int64_t t = esp_timer_get_time();
    while (!i2slink_test_lock(link, &gp, &gl, &gm) && esp_timer_get_time() - t < 2000000) {
        i2slink_read(link, buf, nullptr, 1000);
    }
    if (!i2slink_test_lock(link, &gp, &gl, &gm)) {
        printf("lockstress: no lock to start from\n");
        return;
    }
    i2slink_stats_t s0;
    i2slink_stats(link, &s0);
    printf("lockstress %s %d s: good lock phase %u lane %u marker %u\n", mode, seconds, gp, gl, gm);
    const int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    int64_t nextReport = esp_timer_get_time() + 10000000;
    uint32_t seed = esp_random();
    uint32_t breaks = 0, recovered = 0, timeouts = 0, falseLocks = 0, calls = 0;
    int64_t sumUs = 0, maxUs = 0;
    auto report = [&](const char *what) {
        i2slink_stats_t s;
        i2slink_stats(link, &s);
        printf("lockstress %s %s: breaks %u, recovered %u, timeouts %u, FALSE LOCKS %u, recovery mean %lld us "
               "max %lld us, reads/recovery %.1f, locks %u (fallback %u), framing %u, sio %u\n",
               mode, what, (unsigned)breaks, (unsigned)recovered, (unsigned)timeouts, (unsigned)falseLocks,
               recovered ? (long long)(sumUs / recovered) : 0LL, (long long)maxUs,
               breaks ? (double)calls / breaks : 0.0, (unsigned)(s.locks - s0.locks),
               (unsigned)(s.locks_fallback - s0.locks_fallback), (unsigned)(s.framing - s0.framing),
               (unsigned)counts.sio);
    };
    uint32_t dumps = 0;   // timeouts reported in full (the rest only counted)
    while (esp_timer_get_time() < end) {
        const bool stale = (breaks & 1U) == 0;
        const uint32_t breakSeed = seed++;
        i2slink_test_break_lock(link, breakSeed, stale);
        ++breaks;
        uint8_t bp, bl;
        uint16_t bm;
        i2slink_test_lock(link, &bp, &bl, &bm);   // the planted wrong lock
        i2slink_stats_t sb;
        i2slink_stats(link, &sb);
        uint32_t byStatus[8] = {};                // reads by i2slink_status_t, this attempt
        uint32_t skipped = 0;                     // hint path: hint() false, no read
        const int64_t t0 = esp_timer_get_time();
        bool ok = false;
        while (esp_timer_get_time() - t0 < 200000) {
            i2slink_status_t st = I2SLINK_TIMEOUT;
            ++calls;
            if (path == 0) {
                st = i2slink_peek(link, buf, nullptr);
                ++byStatus[st & 7];
            } else if (path == 1 || i2slink_hint(link)) {
                st = i2slink_read(link, buf, nullptr, 0);
                ++byStatus[st & 7];
            } else {
                ++skipped;
            }
            const bool locked = i2slink_test_lock(link, &p, &l, &m);
            const bool right = locked && p == gp && l == gl && m == gm;
            if (st == I2SLINK_NEW && !right) {
                ++falseLocks;
            }
            if (right) {
                ok = true;
                break;
            }
        }
        const int64_t dt = esp_timer_get_time() - t0;
        // Frames the loop above read never reached the protocol layer (an
        // unacked command would stall main for good): offer the current one
        // again to the service calls below.
        i2slink_test_redeliver(link);
        if (ok) {
            ++recovered;
            sumUs += dt;
            maxUs = std::max(maxUs, dt);
        } else {
            ++timeouts;
            if (dumps < 5) {
                ++dumps;
                uint8_t fp, fl;
                uint16_t fm;
                const bool flocked = i2slink_test_lock(link, &fp, &fl, &fm);
                i2slink_stats_t sa;
                i2slink_stats(link, &sa);
                printf("lockstress %s TIMEOUT #%u (break %u, seed %08x): %s break to phase %u lane %u "
                       "marker %u (right: %u %u %u); after 200 ms %s phase %u lane %u marker %u\n",
                       mode, (unsigned)timeouts, (unsigned)breaks, (unsigned)breakSeed,
                       stale ? "STALE" : "DROPPED", bp, bl, bm, gp, gl, gm,
                       flocked ? "locked" : "unlocked", fp, fl, fm);
                printf("  reads: new %u timeout %u torn %u crc %u nolock %u stalled %u; hint false %u | "
                       "deltas: locks %u framing %u windows %u late %u tripped %u torn %u stale %u crc %u "
                       "peeks %u peeks_torn %u\n",
                       (unsigned)byStatus[I2SLINK_NEW], (unsigned)byStatus[I2SLINK_TIMEOUT],
                       (unsigned)byStatus[I2SLINK_TORN], (unsigned)byStatus[I2SLINK_CRC],
                       (unsigned)byStatus[I2SLINK_NO_LOCK], (unsigned)byStatus[I2SLINK_STALLED],
                       (unsigned)skipped, (unsigned)(sa.locks - sb.locks), (unsigned)(sa.framing - sb.framing),
                       (unsigned)(sa.windows - sb.windows), (unsigned)(sa.late - sb.late),
                       (unsigned)(sa.tripped - sb.tripped), (unsigned)(sa.torn - sb.torn),
                       (unsigned)(sa.stale - sb.stale), (unsigned)(sa.crc_errors - sb.crc_errors),
                       (unsigned)(sa.peeks - sb.peeks), (unsigned)(sa.peeks_torn - sb.peeks_torn));
                i2slink_test_dump(link, "lockstress");
                fflush(stdout);
            }
            // Get back to the right lock for the next round: drop it and read.
            i2slink_test_break_lock(link, seed++, false);
            const int64_t t1 = esp_timer_get_time();
            while (esp_timer_get_time() - t1 < 1000000) {
                i2slink_read(link, buf, nullptr, 1000);
                if (i2slink_test_lock(link, &p, &l, &m) && p == gp && l == gl && m == gm) break;
            }
        }
        while (esp_timer_get_time() - t0 < 50000) {
            service.service();
            printMainOut();   // main's LOG() text (SerialOut) keeps coming meanwhile
        }
        if (esp_timer_get_time() >= nextReport) {
            nextReport += 10000000;
            report("so far");
        }
    }
    report("DONE");
}

// Console "cp <src> <dst>": copy a file between VFS paths (/spiffs, /sd),
// e.g. a disk image from the spiffs to the SD card. Overwrites dst.
void copyFile(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        printf("Sidecar: cp: can't open %s\n", src);
        return;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        printf("Sidecar: cp: can't create %s\n", dst);
        fclose(in);
        return;
    }
    constexpr size_t kChunk = 32 * 1024;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk]);
    const int64_t start = esp_timer_get_time();
    size_t total = 0;
    bool ok = buf != nullptr;
    while (ok) {
        const size_t n = fread(buf.get(), 1, kChunk, in);
        if (n == 0) {
            ok = !ferror(in);
            break;
        }
        ok = fwrite(buf.get(), 1, n, out) == n;
        total += n;
    }
    fclose(in);
    ok = fclose(out) == 0 && ok;
    const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    if (ok) {
        printf("Sidecar: cp: %s -> %s: %u bytes in %d ms\n", src, dst, (unsigned)total, ms);
    } else {
        printf("Sidecar: cp: %s -> %s failed after %u bytes\n", src, dst, (unsigned)total);
        unlink(dst);
    }
}

// Console "gunzip <src> <dst>": inflate a gzip file, e.g. an .atr.gz into
// the plain .atr that a "direct" mount needs (sio.h). Inflates into PSRAM,
// then writes dst. Overwrites dst.
void gunzipFile(const char *src, const char *dst)
{
    struct stat st;
    const int fd = stat(src, &st) == 0 ? open(src, O_RDONLY) : -1;
    if (fd < 0) {
        printf("Sidecar: gunzip: can't open %s\n", src);
        return;
    }
    const GzipReadFn readFile = [fd](uint8_t *buf, size_t len) {
        return static_cast<int>(::read(fd, buf, len));
    };
    const int64_t start = esp_timer_get_time();
    GzipImageInfo info;
    const size_t fileSize = static_cast<size_t>(st.st_size);
    const size_t maxOut = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    uint8_t *out = nullptr;
    bool ok = gzipImageInflate(readFile, fileSize, nullptr, 0, maxOut, info);   // size it
    if (ok) {
        out = static_cast<uint8_t *>(heap_caps_malloc(info.outputSize + 1, MALLOC_CAP_SPIRAM));
        ok = out != nullptr && lseek(fd, 0, SEEK_SET) >= 0 &&
             gzipImageInflate(readFile, fileSize, out, info.outputSize + 1, info.outputSize, info);
    }
    close(fd);
    FILE *f = ok ? fopen(dst, "wb") : nullptr;
    ok = f != nullptr && fwrite(out, 1, info.outputSize, f) == info.outputSize;
    if (f != nullptr && fclose(f) != 0)
        ok = false;
    heap_caps_free(out);
    const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    if (ok) {
        printf("Sidecar: gunzip: %s (%u bytes) -> %s (%u bytes) in %d ms\n", src, (unsigned)fileSize, dst,
               (unsigned)info.outputSize, ms);
    } else {
        printf("Sidecar: gunzip: %s -> %s failed\n", src, dst);
        if (f != nullptr)
            unlink(dst);
    }
}

// Console "ls [dir]": a directory's entries and sizes (default /sd).
void listDir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        printf("Sidecar: ls: can't open %s\n", dir);
        return;
    }
    unsigned count = 0;
    while (const dirent *e = readdir(d)) {
        const std::string path = std::string(dir) + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            printf("  %10s  %s/\n", "", e->d_name);
        else
            printf("  %10ld  %s\n", stat(path.c_str(), &st) == 0 ? (long)st.st_size : -1L, e->d_name);
        ++count;
    }
    closedir(d);
    printf("Sidecar: ls %s: %u entries\n", dir, count);
}
}  // namespace

// Console commands (a line each; the USB console on the board, CSIM_SIDECAR_CMD in csim).
void sidecarAppCommand(const char *line)
{
    printf("Sidecar: command '%s'\n", line);
    if (strcmp(line, "fetch test") == 0) {
        fetchReport(SidecarObject::Test);
    } else if (strcmp(line, "fetch core") == 0 || strcmp(line, "fetch core hash") == 0 ||
               strcmp(line, "fetch core null") == 0) {
        // Main must be in diagnostic mode (diag first): it serves Core only there.
        const CoreSink sink = strcmp(line, "fetch core hash") == 0   ? CoreSink::Hash
                              : strcmp(line, "fetch core null") == 0 ? CoreSink::Null
                                                                     : CoreSink::Store;
        const CoreFetch f = fetchCore(sink);
        printCoreFetchTiming(line, f);
        printf("Sidecar fetch Core from main: link %d, status %d, %u of %u bytes, version %08x, "
               "%s, sha %s, %d ms (%.1f kB/s)\n", (int)f.r.link, (int)f.r.status,
               (unsigned)f.r.received, (unsigned)f.r.size, (unsigned)f.r.version,
               f.r.consistent ? "consistent" : "INCONSISTENT", f.shaOk ? "ok" : "BAD", f.ms,
               f.ms ? f.r.received / 1.024 / f.ms : 0.0);
    } else if (strcmp(line, "corehold on") == 0 || strcmp(line, "corehold off") == 0) {
        coreHold = strcmp(line, "corehold on") == 0;
        printf("Sidecar: corehold %s\n", coreHold ? "on: new core dumps are left in main" : "off");
    } else if (strcmp(line, "ping") == 0) {
        const int64_t start = esp_timer_get_time();
        const bool answered = sidecarProbe(service, 2000000);
        printf("Sidecar: ping main: %s (%d ms)\n", answered ? "answered" : "no answer",
               static_cast<int>((esp_timer_get_time() - start) / 1000));
    } else if (strcmp(line, "diag") == 0 || strcmp(line, "diag ota") == 0 ||
               strcmp(line, "diag panic") == 0) {
        // Staged for main's next boot report (diagnostic mode is decided only
        // there); reset main to apply it now.
        nextBootFlags |= static_cast<uint8_t>(
            strcmp(line, "diag ota") == 0     ? SidecarDiagnosticFlag::FetchFirmware
            : strcmp(line, "diag panic") == 0 ? SidecarDiagnosticFlag::Panic
                                              : SidecarDiagnosticFlag::Diagnostic);
        printf("Sidecar: diagnostic flags %02x staged for main's next boot report; reset main to "
               "apply them\n", nextBootFlags | 0x01);
    } else if (strncmp(line, "lockstress ", 11) == 0) {
        char mode[8] = {};
        int seconds = 30;
        if (sscanf(line + 11, "%7s %d", mode, &seconds) >= 1 &&
            (!strcmp(mode, "peek") || !strcmp(mode, "read") || !strcmp(mode, "hint"))) {
            lockStress(mode, seconds);
        } else {
            printf("Sidecar: usage: lockstress peek|read|hint [seconds]\n");
        }
    } else if (strcmp(line, "link") == 0) {
        printf("Sidecar: link v2: served %u, preempted %u, version mismatches %u\n",
               (unsigned)service.served(), (unsigned)service.preempted(),
               (unsigned)service.versionMismatches());
#ifndef CSIM
    } else if (strcmp(line, "reset") == 0) {
        printf("Sidecar: resetting main\n");
        mainReset::reset();
    } else if (strcmp(line, "report") == 0) {
        sidecarSamplerRequestReport();
    } else if (strncmp(line, "cp ", 3) == 0) {
        char src[64], dst[64];
        if (sscanf(line + 3, "%63s %63s", src, dst) == 2)
            copyFile(src, dst);
        else
            printf("Sidecar: usage: cp <src> <dst>, e.g. cp /spiffs/d1.atr.gz /sd/d1.atr.gz\n");
    } else if (strncmp(line, "gunzip ", 7) == 0) {
        char src[64], dst[64];
        if (sscanf(line + 7, "%63s %63s", src, dst) == 2)
            gunzipFile(src, dst);
        else
            printf("Sidecar: usage: gunzip <src> <dst>, e.g. gunzip /sd/d1.atr.gz /sd/d1.atr\n");
    } else if (strcmp(line, "ls") == 0 || strncmp(line, "ls ", 3) == 0) {
        listDir(line[2] ? line + 3 : "/sd");
    } else if (strcmp(line, "sdformat yes") == 0) {
        sdFormat();
    } else if (strcmp(line, "sdformat") == 0) {
        printf("Sidecar: sdformat erases the SD card (MBR, one FAT32 partition): type \"sdformat yes\"\n");
#endif
    } else {
        printf("Sidecar: commands: fetch test, fetch core [hash|null], corehold on|off, ping, diag [ota|panic], lockstress peek|read|hint [s], link"
#ifndef CSIM
               ", reset, report, sdformat, cp <src> <dst>, gunzip <src> <dst>, ls [dir]"
#endif
               "\n");
    }
}

i2slink_t *sidecarAppLink()
{
    return service.link();
}

void sidecarAppBegin()
{
    mountSpiffs();
#ifndef CSIM
    mountSdCard();
#endif
    sio.setProcFsNodes(&procFsNodes);
    ESP_ERROR_CHECK(service.begin(sidecarLinkConfig));
    nextLog = esp_timer_get_time() + 1000000;
    service.firmwareRefresh();   // the Firmware object's CRC32, now rather than on the first request

}

void sidecarAppPoll()
{
    // Other periodic Sidecar work goes here, alongside service(). SPIFFS
    // garbage-collects inline, inside a write (seconds, at worst): a long
    // stall the host's SIO wait has to cope with, which is a test worth
    // keeping.
    if (halted) {
#ifndef CSIM
        pollConsole();
#endif
        return;
    }
    service.service();
    printMainOut();
    if (bootStatusPending) {
        bootStatusPending = false;
        handleBootReport();
    }
#ifndef CSIM
    pollConsole();
#endif
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
                 "sio=%u cio=%u rx=%u tx=%u locks=%u fb=%u",
                 C(nop), C(sio), C(cio), D(reads_new), D(writes), D(reads_timeout),
                 D(reads_torn), D(reads_crc), D(reads_no_lock), D(reads_stalled), D(framing),
                 D(jumps), (unsigned)counts.nop, (unsigned)counts.sio, (unsigned)counts.cio,
                 (unsigned)s.reads_new, (unsigned)s.writes, (unsigned)s.locks,
                 (unsigned)s.locks_fallback);
#undef D
#undef C
        lastStats = s;
        lastCounts = counts;
    }
}
