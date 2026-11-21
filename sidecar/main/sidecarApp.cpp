// The Sidecar app: see sidecarApp.h.

#include "sidecarApp.h"
#include "activityLed.h"
#include "sidecarNet.h"   // csim: csim/a800/csim_sidecar_net.cpp
#include "../../main/sysConfig.h"   // the boot profile, shared with main (config.load(): main.cpp)

#include "sidecar.h"
#include "diskStitchProc.h"   // ProcFsFile (components/disk_image)
#include "sio.h"   // Sio, DiskImage (components/disk_image)
#include "gzipImage.h"    // hasGzipSuffix(), gzipImageInflate()
#include "gzipStream.h"   // the staged app.bin.gz's token

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "driver/sdmmc_host.h"   // csim: nop stubs (csim/a800/include): no card
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#ifndef CSIM
#include "mainReset.h"
#include "sidecarSampler.h"
#include "hal/usb_serial_jtag_ll.h"
#endif

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include "sidecar_sha256.h"   // console "hash"
#include "sidecarMainFiles.h"   // main's files: the console and the web pages
#ifndef CSIM
#include "freertos/semphr.h"   // sidecarRunInServiceLoop()
#endif
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

// SIO commands by direction, for the activity LED.
bool dcbIsRead(uint8_t c) { return c == 0x52 || c == 0xd2; }
bool dcbIsWrite(uint8_t c) { return c == 0x50 || c == 0xd0 || c == 0x57 || c == 0xd7; }

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
bool mainBootSeen = false;    // a boot report since this app started (handleBootReport())
bool bootCoreNew = false;     // the report showed a core dump not yet fetched
// Flags staged on the console (diag ...) for main's next boot report, one shot.
uint8_t nextBootFlags = 0;
uint32_t storedCoreToken(bool remove);

// -DERASE_CORE=1 (boot.sh -sc --erase-core, for bringing up a board): main's
// core dumps are erased, never fetched. At startup the Sidecar deletes the
// ones stored here; a core in main's boot report (any, fetched before or not)
// puts main in diagnostic mode as usual, then EraseCore and a reset, so main
// boots clean and the run goes on (a staged update included). For a main
// that dumps while it can't talk to this Sidecar yet (a firmware mismatch,
// timing out), and for a core that can't be fetched (it would be asked for
// again every boot).
#if defined(ERASE_CORE) && ERASE_CORE
constexpr bool kEraseCore = true;
#else
constexpr bool kEraseCore = false;
#endif

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

// Console input for main (the console's "main <line>"), handed over when
// main asks (ConsoleIn: main/pbi.cpp's handleSerialInput(), every status
// line, ~2 s). Same task as the console and service(): no locking.
constexpr size_t kMainInBytes = 512;
char mainIn[kMainInBytes];
size_t mainInLength = 0;

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

// The telnet server's client, passed through to main (SidecarChannel::
// Telnet): sidecarTelnet.cpp produces and consumes it, the service loop
// pumps it over the link.
constexpr uint32_t kTelnetRxBytes = 1024, kTelnetTxBytes = 1024;
uint8_t telnetRx[kTelnetRxBytes], telnetTx[kTelnetTxBytes];
SidecarStream telnetStream;
int64_t telnetPumpAfter = 0;   // after a failed pump: try again from here

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

    void handleConsoleIn(const ConsoleInCommand &command, ConsoleInResponse &response) override
    {
        (void)command;
        const size_t n = std::min(mainInLength, ConsoleInResponse::kDataBytes);
        memcpy(response.data, mainIn, n);
        memmove(mainIn, mainIn + n, mainInLength - n);
        mainInLength -= n;
        response.body.length = static_cast<uint16_t>(n);
        response.body.reserved = 0;
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
                      (kEraseCore || bootStatus.coreVersion != storedCoreToken(false));
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
        activityLed(dcbIsWrite(command.body.DCOMND) ? LedActivity::Write
                    : dcbIsRead(command.body.DCOMND) ? LedActivity::Read
                                                     : LedActivity::Command);
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
        activityLed(LedActivity::Command);
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
        activityLed(LedActivity::Command);
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

int64_t nextLog = 0, lastLogAt = 0;
// The stats lines: the first 1 s after start (boot_sidecar.sh waits for
// "sidecar: 1s:" as the loop's ready sign), then every kLogIntervalUs, each
// line's deltas covering its whole interval (the label says how long).
constexpr int64_t kLogIntervalUs = 20 * 1000000;
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
//   rm <path>    delete a file
//   recv <path> <size>, send <path>  file transfer (tools/sidecar_xfer.py)
// No serial flashing here: that's sidecar_serial_flasher, a separate app
// flashed by hand, with the Atari disconnected.
char consoleLine[128];
size_t consoleLen = 0;


// File transfer over this console (tools/sidecar_xfer.py drives both ends):
//   recv <path> <size>  the host sends <size> raw bytes after "XFER READY";
//                       they go to <path>.part, renamed to <path> when all
//                       have come. Raw is safe this way round: nothing else
//                       writes to the Sidecar's input, and USB flow-controls.
//   send <path> [<offset> <length>]
//                       "XFER SEND <length>", then the file (or the range)
//                       as lines "@<offset, 8 hex><base64 of 57 bytes, the
//                       last fewer><CRC-32 of those bytes, 8 hex>": other
//                       output can land on the console too (the Sidecar's
//                       log lines between them, now and then a stray byte
//                       inside one), so the host keeps the lines that check
//                       out and asks again for the ranges it's missing.
// Both end "XFER DONE <size> <sha256> <ms>" (or "XFER FAIL <why>"); the host
// compares the SHA-256 with its own. The service loop keeps running between
// chunks, so the link stays up (main's SIO waits out a slow SD write).
struct Xfer {
    enum class Mode { None, Recv, Send } mode = Mode::None;
    FILE *file = nullptr;
    std::string path, part;
    size_t size = 0, done = 0;
    long base = 0;   // send: the range's start in the file
    int64_t start = 0, lastByte = 0;
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    std::unique_ptr<uint8_t[]> buf;
    size_t fill = 0;
};
Xfer xfer;
constexpr size_t kXferBuf = 16 * 1024;
constexpr int64_t kXferIdleUs = 5'000'000;

void xferFinish(bool ok, const char *why)
{
    uint8_t digest[32];
    size_t digestLen = 0;
    const bool hashed = psa_hash_finish(&xfer.sha, digest, sizeof(digest), &digestLen) == PSA_SUCCESS;
    psa_hash_abort(&xfer.sha);
    if (xfer.file != nullptr && fclose(xfer.file) != 0)
        ok = false, why = "close failed";
    xfer.file = nullptr;
    if (xfer.mode == Xfer::Mode::Recv) {
        if (ok) {
            unlink(xfer.path.c_str());   // FAT's rename won't replace a file
            if (rename(xfer.part.c_str(), xfer.path.c_str()) != 0)
                ok = false, why = "rename failed";
        }
        if (!ok)
            unlink(xfer.part.c_str());
    }
    if (ok && hashed) {
        char hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        printf("\nXFER DONE %u %s %d\n", (unsigned)xfer.done, hex,
               (int)((esp_timer_get_time() - xfer.start) / 1000));
    } else {
        printf("\nXFER FAIL %s after %u of %u bytes\n", why ? why : "hash", (unsigned)xfer.done,
               (unsigned)xfer.size);
    }
    xfer.mode = Xfer::Mode::None;
    xfer.buf.reset();
    sidecarSamplerQuiet(false);
}

bool xferBegin(Xfer::Mode mode, const char *path, size_t size)
{
    if (xfer.mode != Xfer::Mode::None) {
        printf("XFER FAIL busy\n");
        return false;
    }
    xfer = Xfer{};
    xfer.buf.reset(new (std::nothrow) uint8_t[kXferBuf]);
    xfer.path = path;
    xfer.part = xfer.path + ".part";
    xfer.file = fopen(mode == Xfer::Mode::Recv ? xfer.part.c_str() : path, mode == Xfer::Mode::Recv ? "wb" : "rb");
    if (xfer.buf == nullptr || xfer.file == nullptr) {
        if (xfer.file) fclose(xfer.file);
        xfer.file = nullptr;
        printf("XFER FAIL can't open %s\n", mode == Xfer::Mode::Recv ? xfer.part.c_str() : path);
        xfer.buf.reset();
        return false;
    }
    psa_crypto_init();
    psa_hash_setup(&xfer.sha, PSA_ALG_SHA_256);
    // Core 0's sampler would print into the middle of a send's lines (two
    // cores writing one FIFO); a recv's "XFER DONE" must reach the host too.
    sidecarSamplerQuiet(true);
    xfer.mode = mode;
    xfer.size = size;
    xfer.start = xfer.lastByte = esp_timer_get_time();
    return true;
}

// recv: drain the FIFO into the buffer, writing it out as it fills.
void xferRecvPoll()
{
    const int64_t now = esp_timer_get_time();
    bool got = false;
    while (xfer.done + xfer.fill < xfer.size && usb_serial_jtag_ll_rxfifo_data_available()) {
        const size_t room = std::min(kXferBuf - xfer.fill, xfer.size - xfer.done - xfer.fill);
        const int n = usb_serial_jtag_ll_read_rxfifo(xfer.buf.get() + xfer.fill, std::min<size_t>(room, 64));
        if (n <= 0) break;
        xfer.fill += n;
        got = true;
        if (xfer.fill == kXferBuf) break;
    }
    if (got) {
        xfer.lastByte = now;
        activityLed(LedActivity::Transfer);
    }
    const bool last = xfer.done + xfer.fill == xfer.size;
    if (xfer.fill == kXferBuf || (last && xfer.fill > 0)) {
        psa_hash_update(&xfer.sha, xfer.buf.get(), xfer.fill);
        if (fwrite(xfer.buf.get(), 1, xfer.fill, xfer.file) != xfer.fill) {
            xferFinish(false, "write failed");
            return;
        }
        xfer.done += xfer.fill;
        xfer.fill = 0;
    }
    if (xfer.done == xfer.size)
        xferFinish(true, nullptr);
    else if (now - xfer.lastByte > kXferIdleUs)
        xferFinish(false, "timeout");
}

// Straight into the USB-Serial-JTAG TX FIFO, waiting for room: the
// console's own output (no driver) drops what doesn't fit in a few ms, and
// a transfer can't lose lines. False if the host stopped reading (1 s).
bool xferWrite(const char *p, size_t len)
{
    int64_t lastProgress = esp_timer_get_time();
    while (len > 0) {
        const int n = usb_serial_jtag_ll_write_txfifo(reinterpret_cast<const uint8_t *>(p), len);
        usb_serial_jtag_ll_txfifo_flush();
        if (n > 0) {
            p += n;
            len -= n;
            lastProgress = esp_timer_get_time();
        } else if (esp_timer_get_time() - lastProgress > 1'000'000) {
            return false;
        }
    }
    return true;
}

// send: up to ~1 KB of "@<base64>" lines per pass, 57 bytes (76 chars) each.
void xferSendPoll()
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t chunk[57 * 18];
    const size_t n = fread(chunk, 1, std::min(sizeof(chunk), xfer.size - xfer.done), xfer.file);
    if (n > 0) {
        psa_hash_update(&xfer.sha, chunk, n);
        activityLed(LedActivity::Transfer);
    }
    char line[1 + 8 + 76 + 8 + 2];
    for (size_t off = 0; off < n; off += 57) {
        const size_t len = std::min<size_t>(57, n - off);
        char *o = line;
        *o++ = '@';
        o += snprintf(o, 9, "%08lx", (unsigned long)(xfer.base + xfer.done + off));
        for (size_t i = 0; i < len; i += 3) {
            const uint32_t v = chunk[off + i] << 16 | (i + 1 < len ? chunk[off + i + 1] << 8 : 0) |
                               (i + 2 < len ? chunk[off + i + 2] : 0);
            *o++ = b64[v >> 18 & 63];
            *o++ = b64[v >> 12 & 63];
            *o++ = i + 1 < len ? b64[v >> 6 & 63] : '=';
            *o++ = i + 2 < len ? b64[v & 63] : '=';
        }
        o += snprintf(o, 9, "%08lx", (unsigned long)esp_rom_crc32_le(0, chunk + off, len));
        *o++ = '\n';
        if (!xferWrite(line, o - line)) {
            xferFinish(false, "host stopped reading");
            return;
        }
    }
    xfer.done += n;
    if (xfer.done == xfer.size || n == 0)
        xferFinish(!ferror(xfer.file) && xfer.done == xfer.size, "read failed");
}

void xferCommand(const char *line)
{
    char path[96];
    unsigned long size = 0;
    if (strncmp(line, "recv ", 5) == 0 && sscanf(line + 5, "%95s %lu", path, &size) == 2) {
        if (xferBegin(Xfer::Mode::Recv, path, size)) {
            printf("\nXFER READY %lu\n", size);
            if (size == 0)
                xferFinish(true, nullptr);
        }
    } else if (strncmp(line, "send ", 5) == 0 && sscanf(line + 5, "%95s", path) == 1) {
        struct stat st;
        unsigned long offset = 0, length = 0;
        const bool range = sscanf(line + 5, "%*s %lu %lu", &offset, &length) == 2;
        if (stat(path, &st) != 0) {
            printf("\nXFER FAIL no %s\n", path);
        } else if (range && offset + length > (unsigned long)st.st_size) {
            printf("\nXFER FAIL range past the end (%lu bytes)\n", (unsigned long)st.st_size);
        } else if (xferBegin(Xfer::Mode::Send, path, range ? length : static_cast<size_t>(st.st_size))) {
            xfer.base = static_cast<long>(offset);
            if (offset != 0 && fseek(xfer.file, xfer.base, SEEK_SET) != 0) {
                xferFinish(false, "seek failed");
                return;
            }
            printf("\nXFER SEND %lu\n", (unsigned long)xfer.size);
            fflush(stdout);   // ahead of the lines xferWrite() puts in the FIFO itself
            if (xfer.size == 0)
                xferFinish(true, nullptr);
        }
    } else {
        printf("Sidecar: usage: recv <path> <size> | send <path> [<offset> <length>] (tools/sidecar_xfer.py)\n");
    }
}

// The console's input, read straight from the USB-Serial-JTAG receive FIFO:
// without its driver, ESP-IDF's console is output-only, and the driver
// would make printf block whenever no host is listening.
void pollConsole()
{
    if (xfer.mode == Xfer::Mode::Recv) {   // the input is the file's bytes
        xferRecvPoll();
        return;
    }
    if (xfer.mode == Xfer::Mode::Send)
        xferSendPoll();
    uint8_t buf[16];
    int n;
    while ((n = usb_serial_jtag_ll_rxfifo_data_available() ? usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf)) : 0) > 0)
    for (int i = 0; i < n; ++i) {
        const char c = static_cast<char>(buf[i]);
        if (c == '\r' || c == '\n') {
            consoleLine[consoleLen] = 0;
            if (strncmp(consoleLine, "recv ", 5) == 0 || strncmp(consoleLine, "send ", 5) == 0) {
                xferCommand(consoleLine);
                consoleLen = 0;
                if (xfer.mode == Xfer::Mode::Recv) return;   // the rest of the input is file data
                continue;
            }
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
// GPIO18 (RX; was GPIO21, now refresh_ everywhere) <- GPIO46, GPIO48 (BCLK)
// -> GPIO10. Why these:
// - The first rigs used 35/36/37, octal PSRAM pins on an N8R8 module: a
//   Sidecar held in reset held host GPIO21 (also the Atari refresh_ input)
//   low on miner6 board 6. These avoid the octal
//   and flash pins (26-37), USB (19/20), the console UART (43/44), the RGB
//   LED (38/48) and the strapping pins 0, 3 and 45.
// - Main app on this board by mistake: 9 and 10 are address inputs there
//   (never outputs; the bench FAKE_CLOCK setup only pulls them), so neither
//   end fights the host's BCLK or TX. Its GPIO46 drives host GPIO18, an input
//   in every image (FAKE_CLOCK no longer drives it).
// - This app on the host board by mistake: TX lands on a pin only the ESP
//   drives (46 is MPD), and 9 and 10 become inputs on Atari address lines.
//   46 is the only usable output pin with two main-app inputs beside it.
// - 46 is a strapping pin, but with GPIO0 high (a flash boot) it only sets
//   ROM boot logging, and host GPIO18 at the far end at most pulls weakly.
// - BCLK on the outer pin, so only RX runs next to it.
const i2slink_config_t sidecarLinkConfig = {
    .clock = I2SLINK_CLOCK_FOLLOWER,
    .i2s_port = 0,         // I2S_NUM_0
    .bclk_gpio = 10,       // from the host's BCLK (GPIO48)
    .tx_gpio = 46,         // to the host's RX (GPIO18)
    .rx_gpio = 9,          // from the host's TX (GPIO0)
    .sync_timeout_us = 0,  // default: four wire cycles
    .loopback = false,
    // TX frame auto sync: shift our TX until main hears it at bit phase 0,
    // lane 0 (main, the end with the bus loop, doesn't: i2slink.h).
    .tx_auto_sync = true,
    .log = nullptr,   // the Sidecar protocol's (LOG())
};

namespace {
const char *objectName(SidecarObject object)
{
    switch (object) {
    case SidecarObject::Firmware: return "Firmware";
    case SidecarObject::Core: return "Core";
    case SidecarObject::Test: return "Test";
    case SidecarObject::File: return "File";
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
    mainBootSeen = true;
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
    if (kEraseCore) {
        printf("Sidecar: ERASE_CORE: main's core dump %08x (%u bytes) isn't fetched; erasing it\n",
               (unsigned)s.coreVersion, (unsigned)s.coreSize);
        // As sidecarFetch() does: short tries, serving main's own commands
        // between them (a main just in diagnostic mode is sending its log
        // lines: Preempted), within 30 s. One 30 s try timed out (a24
        // carrier, 2026-11-21). A try whose answer was lost may still have
        // erased it: a later one then finds none (Unavailable), which counts.
        SidecarEraseStatus st = SidecarEraseStatus::Unavailable;
        SidecarResult r = SidecarResult::Timeout;
        bool tried = false;
        const int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 30000000) {
            r = service.eraseCore(s.coreVersion, &st, 500000);
            if (r == SidecarResult::Ok) {
                if (st == SidecarEraseStatus::Unavailable && tried) st = SidecarEraseStatus::Erased;
                break;
            }
            if (r == SidecarResult::Preempted) {
                service.serviceOnce();
            } else {
                tried = true;   // sent; its answer didn't come back
            }
        }
        if (r == SidecarResult::Ok && st == SidecarEraseStatus::Erased) {
            printf("Sidecar: ERASE_CORE: erased in main's flash; resetting main\n");
            fflush(stdout);
#ifndef CSIM
            mainReset::reset();
#endif
            return;
        }
        printf("SIDECAR HALT: ERASE_CORE: EraseCore %08x failed: link %d, status %d; main left in "
               "diagnostic mode\n", (unsigned)s.coreVersion, (int)r, (int)st);
        printf("DONE (Sidecar: ERASE_CORE failed)\n");
        fflush(stdout);
        halted = true;
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
// "lockstress align": stress the TX alignment instead (the Sidecar's TX
// shift moved to a random value every 50 ms; main's lock breaks, it relocks
// and reports, and the Sidecar realigns). A recovery is a report of a newer
// generation than the one before the scramble, at phase 0, lane 0; main
// relocks only when it next looks at the link, so SIO traffic paces it: a
// round in which main did no SIO and never recovered counts as idle, not as
// a timeout.
void alignStress(int seconds)
{
    i2slink_t *link = service.link();
    i2slink_stats_t s0;
    i2slink_stats(link, &s0);
    printf("lockstress align %d s: TX shift %u, peer report %08x\n", seconds, (unsigned)s0.tx_shift,
           (unsigned)s0.peer_report);
    const int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    int64_t nextReport = esp_timer_get_time() + 10000000;
    uint32_t seed = esp_random();
    uint32_t breaks = 0, recovered = 0, timeouts = 0, idle = 0, unaligned = 0;
    int64_t sumUs = 0, maxUs = 0;
    auto report = [&](const char *what) {
        i2slink_stats_t s;
        i2slink_stats(link, &s);
        printf("lockstress align %s: breaks %u, recovered %u, timeouts %u, idle %u, unaligned reports %u, "
               "recovery mean %lld us max %lld us, realigns %u, sio %u\n",
               what, (unsigned)breaks, (unsigned)recovered, (unsigned)timeouts, (unsigned)idle,
               (unsigned)unaligned,
               recovered ? (long long)(sumUs / recovered) : 0LL, (long long)maxUs,
               (unsigned)(s.realigns - s0.realigns), (unsigned)counts.sio);
    };
    // The last rounds, printed with a timeout: report before, shift before
    // and after the scramble, the report and shift at the end, outcome.
    struct Round { uint32_t before, shift0, shift1, after, shiftEnd; char outcome; };
    Round history[4] = {};
    while (esp_timer_get_time() < end) {
        i2slink_stats_t sb;
        i2slink_stats(link, &sb);
        uint32_t delay0 = 0, gen0 = 0x100;
        i2slink_report_decode(sb.peer_report, &delay0, &gen0);
        i2slink_test_shift_tx(link, seed++);
        Round &round = history[breaks % 4];
        {
            i2slink_stats_t s1;
            i2slink_stats(link, &s1);
            round = {sb.peer_report, sb.tx_shift, s1.tx_shift, 0, 0, '?'};
        }
        ++breaks;
        const int64_t t0 = esp_timer_get_time();
        const uint32_t sio0 = counts.sio;
        bool ok = false;
        uint32_t lastGen = gen0;
        while (esp_timer_get_time() - t0 < 2000000) {
            service.service();
            printMainOut();
            i2slink_stats_t s;
            i2slink_stats(link, &s);
            uint32_t delay, gen;
            if (i2slink_report_decode(s.peer_report, &delay, &gen) && gen != lastGen) {
                lastGen = gen;
                if (delay != 0xFFFFU && delay % 16U == 0U) {
                    ok = true;
                    break;
                }
                ++unaligned;   // main's first relock, before the realign
            }
        }
        const int64_t dt = esp_timer_get_time() - t0;
        {
            i2slink_stats_t s1;
            i2slink_stats(link, &s1);
            round.after = s1.peer_report;
            round.shiftEnd = s1.tx_shift;
            round.outcome = ok ? 'R' : counts.sio - sio0 < 2 ? 'I' : 'T';
        }
        if (ok) {
            ++recovered;
            sumUs += dt;
            maxUs = std::max(maxUs, dt);
        } else if (counts.sio - sio0 < 2) {
            ++idle;   // main never looked at the link: no relock to report
        } else {
            ++timeouts;
            i2slink_stats_t s;
            i2slink_stats(link, &s);
            printf("lockstress align TIMEOUT #%u: TX shift %u, peer report %08x (before %08x); in the "
                   "round: sio %u, reads new %u, windows %u, framing %u, locks %u, realigns %u, hints %u\n",
                   (unsigned)timeouts, (unsigned)s.tx_shift, (unsigned)s.peer_report, (unsigned)sb.peer_report,
                   (unsigned)(counts.sio - sio0), (unsigned)(s.reads_new - sb.reads_new),
                   (unsigned)(s.windows - sb.windows), (unsigned)(s.framing - sb.framing),
                   (unsigned)(s.locks - sb.locks), (unsigned)(s.realigns - sb.realigns),
                   (unsigned)(s.hints_true - sb.hints_true));
            for (uint32_t k = breaks >= 4 ? breaks - 4 : 0; k < breaks; ++k) {
                const Round &h = history[k % 4];
                printf("  round %u %c: report %08x shift %u -> scrambled %u -> report %08x shift %u\n",
                       (unsigned)k + 1, h.outcome, (unsigned)h.before, (unsigned)h.shift0, (unsigned)h.shift1,
                       (unsigned)h.after, (unsigned)h.shiftEnd);
            }
        }
        while (esp_timer_get_time() - t0 < 50000) {
            service.service();
            printMainOut();
        }
        if (esp_timer_get_time() >= nextReport) {
            nextReport += 10000000;
            report("so far");
        }
    }
    report("DONE");
}

void lockStress(const char *mode, int seconds)
{
    if (strcmp(mode, "align") == 0) {
        alignStress(seconds);
        return;
    }
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

// Files on main (sidecar.h, FileOpen ...): sidecarMainFiles.h. Main serves
// them only in diagnostic mode. Each request is one transaction from here,
// answered at once (main is a plain server: no nesting); main's own traffic
// meanwhile (Preempted) is served and the request retried, as
// sidecarFetch() does. All on the service task.
namespace mainFiles {
namespace {
// Main's SPIFFS can stall a write or a create for seconds (garbage
// collection, once it has seen many writes and deletes: 120 KB uploads went
// from 5 to 16 s, one took 44 s, and a 5 s limit timed out a call).
constexpr uint32_t kTimeoutUs = 30000000;
std::atomic<bool> claimed{false};

SidecarResult call(SidecarCommand command, void *message, void *response, uint32_t timeoutUs)
{
    const int64_t end = esp_timer_get_time() + timeoutUs;
    for (;;) {
        const int64_t left = end - esp_timer_get_time();
        if (left <= 0) {
            return SidecarResult::Timeout;
        }
        const SidecarResult r = service.transact(command, message, response, (uint32_t)left);
        if (r != SidecarResult::Preempted) {
            return r;
        }
        service.serviceOnce();
    }
}

const char *statusText(uint8_t status, int err)
{
    switch (static_cast<SidecarFileStatus>(status)) {
    case SidecarFileStatus::Ok: return "ok";
    case SidecarFileStatus::Failed: return err ? strerror(err) : "failed";
    case SidecarFileStatus::Unavailable: return "no file open on main";
    case SidecarFileStatus::BadRequest: return "bad request";
    case SidecarFileStatus::OutOfOrder: return "chunk out of order";
    }
    return "?";
}

// A result: ok, else *err says why.
bool result(SidecarResult r, uint8_t status, int err, std::string *why)
{
    if (r != SidecarResult::Ok) {
        if (why) *why = "no answer from main";
        return false;
    }
    if (status != 0) {
        if (why) *why = statusText(status, err);
        return false;
    }
    return true;
}

template <typename Command> bool setPath(Command &command, const char *path, std::string *why)
{
    if (strlen(path) >= sizeof(command.data)) {
        if (why) *why = "path too long";
        return false;
    }
    strcpy(reinterpret_cast<char *>(command.data), path);
    return true;
}
}  // namespace

bool available(std::string *why)
{
    if (!mainBootSeen || !(bootReplyFlags & static_cast<uint8_t>(SidecarDiagnosticFlag::Diagnostic))) {
        if (why) *why = "main isn't in diagnostic mode (it serves files only there): console \"diag\", then \"reset\"";
        return false;
    }
    return true;
}

bool claim()
{
    bool expected = false;
    return claimed.compare_exchange_strong(expected, true);
}

void release()
{
    claimed.store(false);
}

bool busy()
{
    return claimed.load();
}

bool open(const char *path, SidecarFileMode mode, uint32_t *size, std::string *why)
{
    FileOpenCommand &command = service.scratchCommand<FileOpenCommand>();
    if (!setPath(command, path, why)) return false;
    command.body.mode = static_cast<uint8_t>(mode);
    FileOpenResponse &response = service.scratchResponse<FileOpenResponse>();
    const SidecarResult r = call(SidecarCommand::FileOpen, &command, &response, kTimeoutUs);
    if (!result(r, response.header.status, response.body.err, why)) return false;
    *size = response.body.size;
    return true;
}

bool close(bool commit, std::string *why)
{
    FileCloseCommand &command = service.scratchCommand<FileCloseCommand>();
    command.body.commit = commit ? 1 : 0;
    FileCloseResponse &response = service.scratchResponse<FileCloseResponse>();
    const SidecarResult r = call(SidecarCommand::FileClose, &command, &response, kTimeoutUs);
    return result(r, response.header.status, response.body.err, why);
}

bool read(uint32_t offset, uint8_t *buf, uint32_t *length, std::string *why)
{
    ReadChunkCommand &command = service.scratchCommand<ReadChunkCommand>();
    command.body.object = static_cast<uint8_t>(SidecarObject::File);
    command.body.offset = offset;
    ReadChunkResponse &response = service.scratchResponse<ReadChunkResponse>();
    const SidecarResult r = call(SidecarCommand::ReadChunk, &command, &response, kTimeoutUs);
    if (r != SidecarResult::Ok || response.header.status != static_cast<uint8_t>(SidecarReadStatus::Ok) ||
        response.body.length > sizeof(response.data)) {
        if (why) {
            char text[64];
            snprintf(text, sizeof(text), "read at %u failed (%s, status %u)", (unsigned)offset,
                     r != SidecarResult::Ok ? "no answer from main" : "main's read", (unsigned)response.header.status);
            *why = text;
        }
        return false;
    }
    memcpy(buf, response.data, response.body.length);
    *length = response.body.length;
    return true;
}

bool write(uint32_t offset, const uint8_t *data, uint32_t length, std::string *why)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        WriteChunkCommand &command = service.scratchCommand<WriteChunkCommand>();
        command.body.offset = offset;
        command.body.length = static_cast<uint16_t>(length);
        memcpy(command.data, data, length);
        WriteChunkResponse &response = service.scratchResponse<WriteChunkResponse>();
        const SidecarResult r = call(SidecarCommand::WriteChunk, &command, &response, kTimeoutUs);
        if (r == SidecarResult::Ok) {
            // A timed-out chunk is sent again: main acknowledges a repeat of
            // its last chunk without writing it twice.
            return result(r, response.header.status, response.body.err, why);
        }
    }
    if (why) *why = "no answer from main";
    return false;
}

bool remove(const char *path, std::string *why)
{
    FilePathCommand &command = service.scratchCommand<FilePathCommand>();
    if (!setPath(command, path, why)) return false;
    FilePathResponse &response = service.scratchResponse<FilePathResponse>();
    const SidecarResult r = call(SidecarCommand::FileRemove, &command, &response, kTimeoutUs);
    return result(r, response.header.status, response.body.err, why);
}

bool hash(const char *path, uint8_t digest[32], uint32_t *size, std::string *why)
{
    FileHashCommand &command = service.scratchCommand<FileHashCommand>();
    if (!setPath(command, path, why)) return false;
    FileHashResponse &response = service.scratchResponse<FileHashResponse>();
    // Main reads the whole file (SPIFFS: about 1 MB/s; main's is 3.2 MB at most).
    const SidecarResult r = call(SidecarCommand::FileHash, &command, &response, 60000000);
    if (!result(r, response.header.status, response.body.err, why)) return false;
    memcpy(digest, response.body.sha256, 32);
    *size = response.body.size;
    return true;
}

bool list(const char *dir, std::vector<Entry> *entries, std::string *why)
{
    uint32_t size = 0;
    if (!open(dir, SidecarFileMode::List, &size, why)) return false;
    std::string text;
    uint8_t buf[kReadChunkBytes];
    uint32_t offset = 0, n = 0;
    bool ok = true;
    while ((ok = read(offset, buf, &n, why)) && n) {
        text.append(reinterpret_cast<const char *>(buf), n);
        offset += n;
    }
    close(false, nullptr);
    if (!ok) return false;
    for (size_t pos = 0; pos < text.size();) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        entries->push_back({line.substr(sp + 1), strtol(line.c_str(), nullptr, 10)});
    }
    return true;
}
}  // namespace mainFiles

// Work handed to the service task (sidecarMainFiles.h). The board's web
// server runs in its own task: one job at a time, the caller waits.
namespace {
#ifndef CSIM
SemaphoreHandle_t jobLock = nullptr, jobDone = nullptr;
std::atomic<const std::function<void()> *> job{nullptr};
#endif

void runServiceJob()
{
#ifndef CSIM
    if (const std::function<void()> *fn = job.load()) {
        (*fn)();
        job.store(nullptr);
        xSemaphoreGive(jobDone);
    }
#endif
}
}  // namespace

void sidecarRunInServiceLoop(const std::function<void()> &fn)
{
#ifdef CSIM
    fn();   // csim's web server is polled from the service loop: already there
#else
    static std::once_flag once;
    std::call_once(once, [] {
        jobLock = xSemaphoreCreateMutex();
        jobDone = xSemaphoreCreateBinary();
    });
    xSemaphoreTake(jobLock, portMAX_DELAY);
    job.store(&fn);
    xSemaphoreTake(jobDone, portMAX_DELAY);
    xSemaphoreGive(jobLock);
#endif
}

namespace {
#ifndef CSIM
// The console's "main:" filespecs: cp/ls/rm/hash. A main: command refuses
// with a hint unless main's last boot went into diagnostic mode, or while
// the web pages hold main's file channel.
bool mainDiagnostic()
{
    std::string why;
    if (!mainFiles::available(&why)) {
        printf("Sidecar: %s\n", why.c_str());
        return false;
    }
    if (mainFiles::busy()) {
        printf("Sidecar: main's files are in use by the web pages; try again when that transfer is done\n");
        return false;
    }
    return true;
}

// "main:/x" -> main, "/x"; "sidecar:/x" or "/x" -> here.
struct FileSpec {
    bool main;
    const char *path;
};
FileSpec fileSpec(const char *spec)
{
    if (strncmp(spec, "main:", 5) == 0) return {true, spec + 5};
    if (strncmp(spec, "sidecar:", 8) == 0) return {false, spec + 8};
    return {false, spec};
}

void reportCopy(const char *src, const char *dst, uint32_t bytes, int64_t t0)
{
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    printf("Sidecar: cp %s %s: %u bytes in %d ms (%.1f kB/s)\n", src, dst, (unsigned)bytes, ms,
           ms ? bytes / 1.024 / ms : 0.0);
}

// main -> here: read main's file chunk by chunk into dst.part, then rename.
void copyFromMain(const char *srcSpec, const char *mainPath, const char *dst, const char *dstSpec)
{
    const int64_t t0 = esp_timer_get_time();
    std::string why;
    uint32_t size = 0;
    if (!mainFiles::open(mainPath, SidecarFileMode::Read, &size, &why)) {
        printf("Sidecar: main:%s: open failed: %s\n", mainPath, why.c_str());
        return;
    }
    const std::string part = std::string(dst) + ".part";
    FILE *out = fopen(part.c_str(), "wb");
    if (!out) {
        printf("Sidecar: cp: can't create %s: %s\n", part.c_str(), strerror(errno));
        mainFiles::close(false, nullptr);
        return;
    }
    uint8_t buf[mainFiles::kReadChunkBytes];
    uint32_t offset = 0;
    bool ok = true;
    for (;;) {
        uint32_t n = 0;
        if (!mainFiles::read(offset, buf, &n, &why)) {
            printf("Sidecar: main:%s: %s\n", mainPath, why.c_str());
            ok = false;
            break;
        }
        if (n == 0) break;
        if (fwrite(buf, 1, n, out) != n) {
            printf("Sidecar: cp: write to %s failed: %s\n", part.c_str(), strerror(errno));
            ok = false;
            break;
        }
        offset += n;
    }
    if (fclose(out) != 0) ok = false;
    mainFiles::close(false, nullptr);
    if (ok && offset != size) {
        printf("Sidecar: cp: main:%s: got %u bytes, expected %u\n", mainPath, (unsigned)offset, (unsigned)size);
        ok = false;
    }
    if (ok) {
        unlink(dst);
        ok = rename(part.c_str(), dst) == 0;
    }
    if (!ok) {
        unlink(part.c_str());
        printf("Sidecar: cp %s %s: FAILED\n", srcSpec, dstSpec);
        return;
    }
    reportCopy(srcSpec, dstSpec, offset, t0);
}

// here -> main: WriteChunk the file to main's <path>.part, then FileClose
// commit (renamed on main).
void copyToMain(const char *src, const char *srcSpec, const char *mainPath, const char *dstSpec)
{
    const int64_t t0 = esp_timer_get_time();
    FILE *in = fopen(src, "rb");
    if (!in) {
        printf("Sidecar: cp: can't open %s: %s\n", src, strerror(errno));
        return;
    }
    std::string why;
    uint32_t size = 0;
    if (!mainFiles::open(mainPath, SidecarFileMode::Write, &size, &why)) {
        printf("Sidecar: main:%s: open failed: %s\n", mainPath, why.c_str());
        fclose(in);
        return;
    }
    uint32_t offset = 0;
    bool ok = true;
    uint8_t buf[mainFiles::kWriteChunkBytes];
    for (;;) {
        const size_t n = fread(buf, 1, sizeof(buf), in);
        if (n == 0) {
            if (ferror(in)) {
                printf("Sidecar: cp: read of %s failed\n", src);
                ok = false;
            }
            break;
        }
        if (!mainFiles::write(offset, buf, n, &why)) {
            printf("Sidecar: cp: main:%s: write at %u: %s\n", mainPath, (unsigned)offset, why.c_str());
            ok = false;
            break;
        }
        offset += n;
    }
    fclose(in);
    if (!mainFiles::close(ok, &why) || !ok) {
        if (!why.empty()) printf("Sidecar: main:%s: close: %s\n", mainPath, why.c_str());
        printf("Sidecar: cp %s %s: FAILED\n", srcSpec, dstSpec);
        return;
    }
    reportCopy(srcSpec, dstSpec, offset, t0);
}

// Console "cp <src> <dst>": either side may be on main ("main:/spiffs/...").
void copyCommand(const char *srcSpec, const char *dstSpec)
{
    const FileSpec src = fileSpec(srcSpec), dst = fileSpec(dstSpec);
    if (!src.main && !dst.main) {
        copyFile(src.path, dst.path);
    } else if (src.main && dst.main) {
        printf("Sidecar: cp: main to main isn't supported (copy it here first)\n");
    } else if (mainDiagnostic()) {
        if (src.main)
            copyFromMain(srcSpec, src.path, dst.path, dstSpec);
        else
            copyToMain(src.path, srcSpec, dst.path, dstSpec);
    }
}

// Console "ls [spec]".
void listCommand(const char *spec)
{
    const FileSpec f = fileSpec(spec);
    if (!f.main) {
        listDir(f.path);
        return;
    }
    if (!mainDiagnostic()) return;
    const char *dir = *f.path ? f.path : "/spiffs";
    std::vector<mainFiles::Entry> entries;
    std::string why;
    if (!mainFiles::list(dir, &entries, &why)) {
        printf("Sidecar: ls main:%s: %s\n", dir, why.c_str());
        return;
    }
    for (const mainFiles::Entry &e : entries) {
        if (e.size == -1)
            printf("  %10s  %s/\n", "", e.name.c_str());
        else
            printf("  %10ld  %s\n", e.size, e.name.c_str());
    }
    printf("Sidecar: ls main:%s: %u entries\n", dir, (unsigned)entries.size());
}

// Console "rm <spec>".
void removeCommand(const char *spec)
{
    const FileSpec f = fileSpec(spec);
    if (!f.main) {
        printf("Sidecar: rm %s: %s\n", f.path, unlink(f.path) == 0 ? "removed" : strerror(errno));
        return;
    }
    if (!mainDiagnostic()) return;
    std::string why;
    printf("Sidecar: rm %s: %s\n", spec, mainFiles::remove(f.path, &why) ? "removed" : why.c_str());
}

// Console "hash <spec>": SHA-256 and size, computed where the file is.
void hashCommand(const char *spec)
{
    const FileSpec f = fileSpec(spec);
    uint8_t digest[32];
    uint32_t size = 0;
    const int64_t t0 = esp_timer_get_time();
    if (!f.main) {
        if (!sidecarHashFile(f.path, digest, &size)) {
            printf("Sidecar: hash %s: %s\n", spec, strerror(errno));
            return;
        }
    } else {
        if (!mainDiagnostic()) return;
        std::string why;
        if (!mainFiles::hash(f.path, digest, &size, &why)) {
            printf("Sidecar: hash %s: %s\n", spec, why.c_str());
            return;
        }
    }
    char hex[65];
    sidecarHexDigest(digest, hex);
    printf("Sidecar: hash %s: %s %u bytes (%d ms)\n", spec, hex, (unsigned)size,
           (int)((esp_timer_get_time() - t0) / 1000));
}
#endif  // CSIM
}  // namespace

bool sidecarHashFile(const char *path, uint8_t digest[32], uint32_t *size)
{
    FILE *in = fopen(path, "rb");
    if (!in) {
        return false;
    }
    SidecarSha256 sha;
    uint8_t buf[1024];
    size_t n;
    *size = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        sha.update(buf, n);
        *size += n;
    }
    fclose(in);
    sha.finish(digest);
    return true;
}

void sidecarHexDigest(const uint8_t digest[32], char hex[65])
{
    for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
}

// Console commands (a line each; the USB console on the board, CSIM_SIDECAR_CMD in csim).
void sidecarAppCommand(const char *line)
{
    printf("Sidecar: command '%s'\n", line);
    if (strncmp(line, "main ", 5) == 0) {
        // A line for main's console, as if typed there (e.g. tools/halt.sh's
        // "main exit 2"): main picks it up with its next status line.
        const size_t n = strlen(line + 5);
        if (mainInLength + n + 1 > kMainInBytes) {
            printf("Sidecar: main console input full (%u bytes waiting); dropped\n", (unsigned)mainInLength);
        } else {
            memcpy(mainIn + mainInLength, line + 5, n);
            mainIn[mainInLength + n] = '\n';
            mainInLength += n + 1;
            printf("Sidecar: queued for main's console: '%s'\n", line + 5);
        }
    } else if (strcmp(line, "fetch test") == 0) {
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
    } else if (strcmp(line, "slottune on") == 0 || strcmp(line, "slottune off") == 0) {
        i2slink_test_slot_tune(service.link(), strcmp(line, "slottune on") == 0);
        printf("Sidecar: slot tuning %s\n", strcmp(line, "slottune on") == 0 ? "on" : "off");
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
            (!strcmp(mode, "peek") || !strcmp(mode, "read") || !strcmp(mode, "hint") ||
             !strcmp(mode, "align"))) {
            lockStress(mode, seconds);
        } else {
            printf("Sidecar: usage: lockstress peek|read|hint|align [seconds]\n");
        }
    } else if (strcmp(line, "net") == 0) {
        sidecarNetReport();
    } else if (strcmp(line, "telnet") == 0) {
        printf("Sidecar telnet stream: %s, opens %u, closes %u, rx %u (dropped %u), tx %u, queued %u\n",
               telnetStream.isOpen() ? "open" : "closed", (unsigned)telnetStream.opens(),
               (unsigned)telnetStream.closes(), (unsigned)telnetStream.rxBytes(),
               (unsigned)telnetStream.rxDropped(), (unsigned)telnetStream.txBytes(),
               (unsigned)telnetStream.txQueued());
    } else if (strcmp(line, "telnet open") == 0) {
        // A telnet session from the console (no network needed): main's
        // screen text comes back here.
        telnetStream.open();
    } else if (strcmp(line, "telnet close") == 0) {
        telnetStream.close();
    } else if (strncmp(line, "telnet send ", 12) == 0) {
        // Type a line at main (a newline at the end: the Atari's EOL).
        const std::string text = std::string(line + 12) + "\n";
        const uint32_t n = telnetStream.write(reinterpret_cast<const uint8_t *>(text.data()), text.size());
        printf("Sidecar telnet: queued %u of %u bytes\n", (unsigned)n, (unsigned)text.size());
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
        char src[96], dst[96];
        if (sscanf(line + 3, "%95s %95s", src, dst) == 2)
            copyCommand(src, dst);
        else
            printf("Sidecar: usage: cp <src> <dst>, e.g. cp /sd/d1.atr.gz main:/spiffs/d1.atr.gz "
                   "(\"main:\" a file on main, in diagnostic mode; no prefix or \"sidecar:\" here)\n");
    } else if (strncmp(line, "gunzip ", 7) == 0) {
        char src[64], dst[64];
        if (sscanf(line + 7, "%63s %63s", src, dst) == 2)
            gunzipFile(src, dst);
        else
            printf("Sidecar: usage: gunzip <src> <dst>, e.g. gunzip /sd/d1.atr.gz /sd/d1.atr\n");
    } else if (strncmp(line, "rm ", 3) == 0) {
        removeCommand(line + 3);
    } else if (strcmp(line, "ls") == 0 || strncmp(line, "ls ", 3) == 0) {
        listCommand(line[2] ? line + 3 : "/sd");
    } else if (strncmp(line, "hash ", 5) == 0) {
        hashCommand(line + 5);
    } else if (strcmp(line, "sdformat yes") == 0) {
        sdFormat();
    } else if (strcmp(line, "sdformat") == 0) {
        printf("Sidecar: sdformat erases the SD card (MBR, one FAT32 partition): type \"sdformat yes\"\n");
#endif
    } else {
        printf("Sidecar: commands: fetch test, fetch core [hash|null], corehold on|off, ping, diag [ota|panic], lockstress peek|read|hint [s], link, net, telnet [open|close|send <text>]"
#ifndef CSIM
               ", reset, report, sdformat, cp <src> <dst>, gunzip <src> <dst>, ls [dir], rm <path>, hash <path>, recv <path> <size>, send <path>"
               " (cp/ls/rm/hash paths: \"main:/spiffs/...\" for main's files, in diagnostic mode)"
#endif
               "\n");
    }
}

i2slink_t *sidecarAppLink()
{
    return service.link();
}

void sidecarAppReportTiming()
{
    const SidecarPeer::ResponseTimes rt = service.responseTimes();
    printf("Sidecar response times (us), served %u:", (unsigned)service.served());
    for (size_t b = 0; b < 14; ++b) {
        if (rt.hist[b]) printf(" <%u:%u", 8u << b, (unsigned)rt.hist[b]);
    }
    printf("\n");
}

void sidecarAppBegin()
{
    mountSpiffs();
#ifndef CSIM
    mountSdCard();
#endif
    if (kEraseCore) {
        const uint32_t had = storedCoreToken(false);
        storedCoreToken(true);
        printf("Sidecar: ERASE_CORE: main's core dumps are erased, not fetched%s\n",
               had ? "; deleted the stored one" : "");
    }
    activityLedBegin();
    sio.setProcFsNodes(&procFsNodes);
    sio.setNetworkUp(sidecarNetUp);   // smb: mounts: WiFi with the profile's settings
    telnetStream.init(static_cast<uint8_t>(SidecarChannel::Telnet), telnetRx, kTelnetRxBytes, telnetTx,
                      kTelnetTxBytes);
    service.attachStream(telnetStream);
    ESP_ERROR_CHECK(service.begin(sidecarLinkConfig));
    // The network, from this app's own copy of the boot profile (main's
    // BOOT_CONFIG: both apps build main/sysConfig.cpp).
    SidecarNetSettings net;
    net.wifi = config.enableWifi;
    net.mdns = config.enableMdns;
    net.telnetPort = config.telnetServerPort;
    net.httpPort = config.httpServerPort;
    net.connectDelaySec = config.wifiConnectDelay > 0 ? config.wifiConnectDelay : 0;
    net.ssid = config.wifiSSID;
    net.password = config.wifiPassword;
    net.hostname = config.wifiHostname;
    sidecarNetConfigure(net, telnetStream);
#ifdef SLOT_TUNE
    // -DSLOT_TUNE=0 (boot_sidecar.sh SIDECAR_DEFINES): slot tuning off from boot.
    i2slink_test_slot_tune(service.link(), SLOT_TUNE != 0);
#endif
    {
        // -DSLOT_FAST_US=<us>, -DSLOT_TRACE=1 (csim: CSIM_SLOT_FAST_US, CSIM_SLOT_TRACE).
        unsigned fastUs = 256;
        bool trace = false;
#ifdef SLOT_FAST_US
        fastUs = SLOT_FAST_US;
#endif
#ifdef SLOT_TRACE
        trace = SLOT_TRACE != 0;
#endif
#ifdef CSIM
        if (const char *e = getenv("CSIM_SLOT_FAST_US")) fastUs = (unsigned)atoi(e);
        if (getenv("CSIM_SLOT_TRACE")) trace = true;
#endif
        i2slink_test_slot_config(service.link(), fastUs, trace);
    }
#ifdef CSIM
    // Experiment: CSIM_SYNC_MARKER=<even n> syncs the whole frame to main's
    // ring position n (i2slink_test_sync_marker()).
    if (const char *e = getenv("CSIM_SYNC_MARKER")) {
        i2slink_test_sync_marker(service.link(), atoi(e));
    }
    // CSIM_SLOT_TUNE=0: slot tuning off (on by default).
    if (const char *e = getenv("CSIM_SLOT_TUNE")) {
        i2slink_test_slot_tune(service.link(), atoi(e) != 0);
    }
#endif
    lastLogAt = esp_timer_get_time();
    nextLog = lastLogAt + 1000000;
    service.firmwareRefresh();   // the Firmware object's CRC32, now rather than on the first request

}

#ifndef CSIM
// The link's health for whoever is bringing boards up: a line when the lock
// comes or goes, and while there's none, one every 5 s with what the link's
// pins are doing (i2slink_pin_activity(): BCLK from main, main's data on RX)
// and a hint at the fault. Checked once a second. A locked link with no new
// frame for 5 s is quiet unless its BCLK has stopped (an idle link is
// normal); the pins are measured (4 ms) only then and while unlocked.
struct LinkWatch {
    bool started = false, locked = false;
    int64_t since = 0, nextCheck = 0, nextReport = 0, lastNewAt = 0;
    uint32_t lastNew = 0;
} linkWatch;

void linkWatchReport(const char *what, int64_t now)
{
    i2slink_pin_activity_t a = {};
    if (i2slink_pin_activity(sidecarLinkConfig.bclk_gpio, sidecarLinkConfig.rx_gpio, 0, &a) != ESP_OK) {
        printf("i2slink: %s for %d s (pins not measured: no PCNT unit)\n", what,
               (int)((now - linkWatch.since) / 1000000));
        return;
    }
    printf("i2slink: %s for %d s: BCLK %u.%02u MHz (GPIO%d; expect %u.%02u), RX %u edges/s (GPIO%d): %s\n",
           what, (int)((now - linkWatch.since) / 1000000), (unsigned)(a.bclk_hz / 1000000),
           (unsigned)(a.bclk_hz / 10000 % 100), sidecarLinkConfig.bclk_gpio,
           (unsigned)(32U * I2SLINK_FRAMES_PER_SECOND / 1000000), (unsigned)(32U * I2SLINK_FRAMES_PER_SECOND / 10000 % 100),
           (unsigned)a.rx_edges_per_s, sidecarLinkConfig.rx_gpio, i2slink_pin_diagnosis(&a));
}

void linkWatchPoll(int64_t now)
{
    LinkWatch &w = linkWatch;
    if (!w.started) {
        w.started = true;
        w.since = w.lastNewAt = now;
        w.nextReport = now + 5000000;
        // The first check 2 s in: a lock found in the first fraction of a
        // second (main already running) would be announced before a capture
        // has the console again (the USB port re-enumerates after a reset).
        w.nextCheck = now + 2000000;
    }
    if (now < w.nextCheck) {
        return;
    }
    w.nextCheck = now + 1000000;
    i2slink_stats_t s;
    i2slink_stats(service.link(), &s);
    if (s.reads_new != w.lastNew) {
        w.lastNew = s.reads_new;
        w.lastNewAt = now;
    }
    if (s.locked != w.locked) {
        if (s.locked) {
            printf("i2slink: LOCKED after %d.%d s (phase %u lane %u marker %u, %u frames/s)\n",
                   (int)((now - w.since) / 1000000), (int)((now - w.since) / 100000 % 10), s.bit_phase,
                   s.byte_lane, s.marker_offset, (unsigned)I2SLINK_FRAMES_PER_SECOND);
        } else {
            printf("i2slink: lock LOST\n");
            w.nextReport = now + 5000000;
        }
        w.locked = s.locked;
        w.since = now;
    }
    if (now < w.nextReport) {
        return;
    }
    w.nextReport = now + 5000000;
    if (!s.locked) {
        linkWatchReport("NO LOCK", now);
    } else if (now - w.lastNewAt >= 5000000) {
        i2slink_pin_activity_t a = {};
        if (i2slink_pin_activity(sidecarLinkConfig.bclk_gpio, sidecarLinkConfig.rx_gpio, 0, &a) == ESP_OK &&
            a.bclk_hz == 0) {
            printf("i2slink: locked, but NO BCLK and no frame for %d s: main stopped its link (reset, "
                   "crashed, or the BCLK wire)\n", (int)((now - w.lastNewAt) / 1000000));
        }
    }
}
#endif

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
    static int64_t startupReportAt = 0;   // the startup summary, 5 s after main's boot report
    if (bootStatusPending) {
        bootStatusPending = false;
        handleBootReport();
        startupReportAt = esp_timer_get_time() + 5000000;
    }
    // The telnet stream: one frame per pass (main serves our commands in its
    // link poll); after a miss, wait before trying again.
    if (telnetStream.pending() && esp_timer_get_time() >= telnetPumpAfter &&
        !telnetStream.pump(service, 300000)) {
        telnetPumpAfter = esp_timer_get_time() + 100000;
    }
    sidecarNetPoll(telnetStream);
    runServiceJob();   // the web pages' work for main's files (sidecarRunInServiceLoop())
#ifndef CSIM
    pollConsole();
#endif
    const int64_t now = esp_timer_get_time();
    activityLedPoll(now, service.link());
    if (startupReportAt && now >= startupReportAt) {
        // The link's initial conditions, for tying a later failure (a soak)
        // to them: this end's lock, its TX shift, main's report of its own
        // lock, and what slot tuning has done so far.
        startupReportAt = 0;
        i2slink_stats_t s;
        i2slink_stats(service.link(), &s);
        uint32_t delay = 0, gen = 0;
        const bool ok = i2slink_report_decode(s.peer_report, &delay, &gen);
        printf("Sidecar: startup: lock %s phase %u lane %u marker %u | TX shift %u, realigns %u | main's lock %s "
               "marker %u phase/lane %u | moves made %u | slot samples %u tunes %u, "
               "last window wait us lo %u mean %u hi %u, response med %u\n",
               s.locked ? "locked" : "UNLOCKED", s.bit_phase, s.byte_lane, s.marker_offset, (unsigned)s.tx_shift,
               (unsigned)s.realigns, ok ? "ok" : "BAD",
               (unsigned)((delay / 8U + I2SLINK_VSYNC_BYTES) % I2SLINK_CYCLE_BYTES), (unsigned)(delay % 16U),
               (unsigned)s.moves, (unsigned)s.slot_samples, (unsigned)s.slot_tunes, s.slot_wait_lo, s.slot_wait_mean,
               s.slot_wait_hi, s.slot_resp_med);
    }
#ifndef CSIM
    linkWatchPoll(now);
#endif
    if (now >= nextLog) {
        nextLog = now + kLogIntervalUs;
        const unsigned secs = static_cast<unsigned>((now - lastLogAt + 500000) / 1000000);
        lastLogAt = now;
        // Deltas over the interval (secs), then totals.
        i2slink_stats_t s;
        i2slink_stats(service.link(), &s);
#define D(f) (unsigned)(s.f - lastStats.f)
#define C(f) (unsigned)(counts.f - lastCounts.f)
        ESP_LOGI(TAG, "%us: nop=%u sio=%u cio=%u | frames rx=%u tx=%u | reads{timeout=%u "
                 "torn=%u crc=%u nolock=%u stalled=%u} framing=%u jumps=%u | total nop=%u "
                 "sio=%u cio=%u rx=%u tx=%u locks=%u fb=%u",
                 secs, C(nop), C(sio), C(cio), D(reads_new), D(writes), D(reads_timeout),
                 D(reads_torn), D(reads_crc), D(reads_no_lock), D(reads_stalled), D(framing),
                 D(jumps), (unsigned)counts.nop, (unsigned)counts.sio, (unsigned)counts.cio,
                 (unsigned)s.reads_new, (unsigned)s.writes, (unsigned)s.locks,
                 (unsigned)s.locks_fallback);
        {
            uint32_t delay = 0, gen = 0;
            const bool ok = i2slink_report_decode(s.peer_report, &delay, &gen);
            ESP_LOGI(TAG, "%us sync: target %d shift %u realigns +%u (%u) | main report %s delay %u "
                     "(marker %u phase/lane %u) gen %u torn %u",
                     secs, (int)s.sync_target, (unsigned)s.tx_shift, D(realigns), (unsigned)s.realigns,
                     ok ? "ok" : "BAD", (unsigned)delay,
                     (unsigned)((delay / 8U + I2SLINK_VSYNC_BYTES) % I2SLINK_CYCLE_BYTES), (unsigned)(delay % 16U),
                     (unsigned)gen, (unsigned)s.peer_torn);
            {
                static SidecarPeer::ResponseTimes lastRt = {};
                const SidecarPeer::ResponseTimes rt = service.responseTimes();
                {
                    char line[200];
                    int len = 0;
                    for (size_t b = 0; b < 14; ++b) {
                        const unsigned c = (unsigned)(rt.hist[b] - lastRt.hist[b]);
                        if (c) len += snprintf(line + len, sizeof(line) - len, " <%u:%u", 8u << b, c);
                    }
                    if (len) ESP_LOGI(TAG, "%us response times (us):%s", secs, line);
                }
                lastRt = rt;
            }
            ESP_LOGI(TAG, "%us moves: announced +%u made +%u dropped +%u | followed +%u", secs,
                     D(moves_announced), D(moves), D(moves_dropped), D(moves_followed));
            if (s.slot_tune) {
                ESP_LOGI(TAG, "%us slot: samples +%u misses +%u unknown +%u tunes %u | wait us lo %u mean %u "
                         "med %u hi %u, response med %u us",
                         secs, D(slot_samples), D(slot_misses), D(slot_unknown), (unsigned)s.slot_tunes,
                         s.slot_wait_lo, s.slot_wait_mean, s.slot_wait_med, s.slot_wait_hi, s.slot_resp_med);
            }
        }
#undef D
#undef C
        lastStats = s;
        lastCounts = counts;
    }
}
