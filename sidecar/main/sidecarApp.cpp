// The Sidecar app: see sidecarApp.h.

#include "sidecarApp.h"

#include "sidecar.h"
#include "diskStitchProc.h"   // ProcFsFile (components/disk_image)
#include "sio.h"   // Sio, DiskImage (components/disk_image)

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
#include "mainFlasher.h"
#include "sidecarSampler.h"
#include "hal/usb_serial_jtag_ll.h"
#endif

#include <algorithm>
#ifndef CSIM
#include "psa/crypto.h"   // SHA-256 of a fetched core dump
#endif
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
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
uint32_t lastCoreVersion();

class AtariService : public SidecarService {
protected:
    void handleNop() override { ++counts.nop; }

    // Decide here, in the reply: main enters diagnostic mode at boot or not
    // at all. A core dump not yet fetched means main just crashed:
    // Diagnostic (then sidecarAppPoll() fetches it). Plus any flags staged
    // on the console for this boot.
    void handleBootStatus(const BootStatusCommand &command, BootStatusResponse &response) override
    {
        bootStatus = command.body;
        bootStatus.git[sizeof(bootStatus.git) - 1] = 0;
        bootCoreNew = (bootStatus.flags & static_cast<uint8_t>(SidecarBootFlag::CorePresent)) &&
                      bootStatus.coreVersion != lastCoreVersion();
        uint8_t flags = nextBootFlags;
        nextBootFlags = 0;
        if (bootCoreNew) {
            flags |= static_cast<uint8_t>(SidecarDiagnosticFlag::Diagnostic);
        }
        if (flags) {
            flags |= static_cast<uint8_t>(SidecarDiagnosticFlag::Diagnostic);
        }
        response.body.diagnostic = flags;
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
    static constexpr const char *kFirmwarePath = "/spiffs/main/app.bin";
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

// SPI mode: the card enters it when it sees CMD0 with CS (DAT3) low, even
// after the SD-mode attempt, and stays in it until power-cycled. SPI2 is
// otherwise unused on the Sidecar.
constexpr spi_host_device_t kSdSpiHost = SPI2_HOST;

esp_err_t trySdSpiMount(int maxFreqKhz)
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
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;   // never reformat a user's card
    mount.max_files = 5;
    err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &mount, &sdCard);
    if (err != ESP_OK)
        spi_bus_free(kSdSpiHost);
    return err;
}

// Mount the card's FAT volume at /sd if there is one. No card, a card that
// won't initialize or one without a FAT volume is fine: the Sidecar just
// runs without it.
void mountSdCard()
{
    const int64_t start = esp_timer_get_time();
    sdMode = "4-bit SD";
    esp_err_t err = trySdMount(SDMMC_FREQ_HIGHSPEED);
    // No card shows up as a timeout; anything else may be the 40 MHz clock
    // (long wires, a slow card), so try once more at the default 20 MHz.
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
        err = trySdMount(SDMMC_FREQ_DEFAULT);
    // Then SPI mode (an SPI-only breakout times out in SD mode too), again
    // slower if 20 MHz isn't clean.
    if (err != ESP_OK) {
        sdMode = "SPI";
        err = trySdSpiMount(SDMMC_FREQ_DEFAULT);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
            err = trySdSpiMount(4000);   // kHz
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

int64_t nextLog = 0;
Counts lastCounts = {};
i2slink_stats_t lastStats = {};

#ifndef CSIM
// Console commands, typed on the Sidecar's USB serial console (a line each):
//   flash [app]  write the main board's firmware from /spiffs/main/ (all
//                four images, or just the app), verify, reboot it
//   reset        reset the main board into its firmware
//   dl           reset the main board into ROM download mode and leave it
//   buscheck     phi2/D0/D1 activity now: reset, dl and flash all refuse on
//                any transition (a running Atari, or a FAKE_CLOCK build)
//   buscheck selftest  prove that check: a square wave on the D0 wire must
//                be caught (only on an idle bus)
//   report       print the bus sampler's histograms now
// The link's RX wire is the main board's IO0: flashing borrows it (the link
// sees noise for those seconds).
const mainFlasher::Image kMainImages[] = {
    {"/spiffs/main/bootloader.bin", 0x0},
    {"/spiffs/main/partition-table.bin", 0x8000},
    {"/spiffs/main/ota_data_initial.bin", 0xe000},
    {"/spiffs/main/app.bin", 0x10000},
};
char consoleLine[64];
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
// The last core dump token the Sidecar acted on, kept across Sidecar
// restarts, so an old core dump doesn't stop main on every boot.
constexpr const char *kLastCorePath = "/spiffs/lastcore";
uint32_t lastCoreVersion()
{
    uint32_t v = 0;
    if (FILE *f = fopen(kLastCorePath, "rb")) {
        if (fread(&v, sizeof(v), 1, f) != 1) v = 0;
        fclose(f);
    }
    return v;
}
void setLastCoreVersion(uint32_t v)
{
    if (FILE *f = fopen(kLastCorePath, "wb")) {
        fwrite(&v, sizeof(v), 1, f);
        fclose(f);
    }
}

// Fetch main's core dump (main must be in diagnostic mode: it only serves
// Core there) and check it: all of it, the same token before and after, and
// the SHA-256 IDF stored in its last 32 bytes (over everything before them)
// recomputed here. Where it goes: nowhere yet. The plan is SD card or
// Sidecar flash once there's room (dumps run to 2.9 MB); until then
// boot.sh's crash bundle reads it from main's flash, as it does now.
struct CoreFetch {
    SidecarFetchResult r;
    bool shaOk = false;
    int ms = 0;
};
CoreFetch fetchCore()
{
    CoreFetch f;
    const int64_t start = esp_timer_get_time();
#ifndef CSIM
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
            uint8_t joined[sizeof(tail) + ReadChunkResponse::kDataBytes];
            memcpy(joined, tail, tailLen);
            memcpy(joined + tailLen, data, length);
            const uint32_t n = tailLen + length;
            const uint32_t keep = std::min<uint32_t>(n, sizeof(tail));
            if (n > keep) psa_hash_update(&sha, joined, n - keep);
            memcpy(tail, joined + n - keep, keep);
            tailLen = keep;
            total = offset + length;
#else
            (void)offset; (void)data; (void)length;
#endif
            return true;
        });
#ifndef CSIM
    uint8_t computed[32];
    size_t computedLen = 0;
    f.shaOk = psa_hash_finish(&sha, computed, sizeof(computed), &computedLen) == PSA_SUCCESS &&
              total > sizeof(tail) && tailLen == sizeof(tail) &&
              memcmp(computed, tail, sizeof(tail)) == 0;
#endif
    f.ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    return f;
}

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
    if (bootReplyFlags & static_cast<uint8_t>(SidecarDiagnosticFlag::FetchFirmware)) {
        printf("Sidecar: main will fetch and install Firmware (OTA); serving it\n");
        return;   // a core fetch would collide with main's fetch; it can wait for the next boot
    }
    if (!bootCoreNew) {
        return;
    }
    printf("Sidecar: new core dump %08x: main just crashed; it's in diagnostic mode; fetching\n",
           (unsigned)s.coreVersion);
    const CoreFetch f = fetchCore();
    const bool ok = f.r.link == SidecarResult::Ok && f.r.status == SidecarReadStatus::Ok &&
                    f.r.consistent && f.shaOk && f.r.version == s.coreVersion;
    printf("Sidecar: core dump %08x: %u of %u bytes, %s, sha %s, %d ms (%.1f kB/s)\n",
           (unsigned)f.r.version, (unsigned)f.r.received, (unsigned)f.r.size,
           f.r.consistent ? "consistent" : "INCONSISTENT", f.shaOk ? "ok" : "BAD", f.ms,
           f.ms ? f.r.received / 1.024 / f.ms : 0.0);
    if (ok) {
        setLastCoreVersion(s.coreVersion);
    }
    // Not stored anywhere yet (no room): see fetchCore().
    printf("SIDECAR HALT: main crashed (reset reason %d, git %s); core dump %08x %s; not stored "
           "(no storage yet); main left in diagnostic mode; reset both to continue\n",
           s.resetReason, s.git, (unsigned)s.coreVersion, ok ? "fetched and verified" : "FETCH FAILED");
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
        }
        if (esp_timer_get_time() >= nextReport) {
            nextReport += 10000000;
            report("so far");
        }
    }
    report("DONE");
}
}  // namespace

// Console commands (a line each; the USB console on the board, CSIM_SIDECAR_CMD in csim).
void sidecarAppCommand(const char *line)
{
    printf("Sidecar: command '%s'\n", line);
    if (strcmp(line, "fetch test") == 0) {
        fetchReport(SidecarObject::Test);
    } else if (strcmp(line, "fetch core") == 0) {
        // Main must be in diagnostic mode (diag first): it serves Core only there.
        const CoreFetch f = fetchCore();
        printf("Sidecar fetch Core from main: link %d, status %d, %u of %u bytes, version %08x, "
               "%s, sha %s, %d ms (%.1f kB/s)\n", (int)f.r.link, (int)f.r.status,
               (unsigned)f.r.received, (unsigned)f.r.size, (unsigned)f.r.version,
               f.r.consistent ? "consistent" : "INCONSISTENT", f.shaOk ? "ok" : "BAD", f.ms,
               f.ms ? f.r.received / 1.024 / f.ms : 0.0);
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
    } else if (strcmp(line, "flash") == 0 || strcmp(line, "flash app") == 0) {
        const bool appOnly = strcmp(line, "flash app") == 0;
        // The link stays up: its RX listens on GPIO9 (main IO0) through the
        // GPIO matrix, and the flasher drives the pin open drain beside
        // that, then hands it back as an input. (Stopping the link instead
        // hung in i2slink_deinit() with no host BCLK running.)
        mainFlasher::flashImages(appOnly ? &kMainImages[3] : kMainImages, appOnly ? 1 : 4);
    } else if (strcmp(line, "reset") == 0) {
        mainFlasher::resetMain();
    } else if (strcmp(line, "dl") == 0) {
        if (mainFlasher::enterDownloadMode() == ESP_OK)
            printf("Sidecar: main board in ROM download mode\n");
    } else if (strcmp(line, "buscheck") == 0) {
        mainFlasher::checkBus();
    } else if (strcmp(line, "buscheck selftest") == 0) {
        mainFlasher::checkBusSelfTest();
    } else if (strcmp(line, "report") == 0) {
        sidecarSamplerRequestReport();
#endif
    } else {
        printf("Sidecar: commands: fetch test, fetch core, ping, diag [ota|panic], lockstress peek|read|hint [s], link"
#ifndef CSIM
               ", flash [app], reset, dl, buscheck [selftest], report"
#endif
               "\n");
    }
}

bool sidecarAppBusy()
{
    return service.busy();
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
