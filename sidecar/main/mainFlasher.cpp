// The Sidecar's serial flasher for the main board: see mainFlasher.h.
//
// The ESP32-S3 ROM loader protocol (as esptool speaks it to the ROM, no
// stub): SLIP-framed packets
//   request:  0x00, cmd, u16 size, u32 checksum, data
//   response: 0x01, cmd, u16 size, u32 value, data (ends in status bytes:
//             status != 0 is a failure, the next byte its error code)
// with SYNC, SPI_ATTACH, SPI_SET_PARAMS, CHANGE_BAUDRATE, FLASH_BEGIN /
// FLASH_DATA / FLASH_END and SPI_FLASH_MD5. The ROM writes uncompressed
// 1 KB blocks; FLASH_BEGIN erases the whole region first.

#include "mainFlasher.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_rom_md5.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace mainFlasher {
namespace {

constexpr gpio_num_t kEnGpio = GPIO_NUM_11;
constexpr gpio_num_t kBootGpio = GPIO_NUM_9;
constexpr int kTxGpio = 12, kRxGpio = 13;
constexpr uart_port_t kUart = UART_NUM_1;
constexpr int kRomBaud = 115200;
// After sync: 2 Mbaud first (bench pair: app 16 s vs 21 s at 921600; the
// carrier boards' 1 k series resistors and the pins' capacitance make a
// ~50 ns edge, well inside its 500 ns bit), then 921600 if anything fails at
// that rate. The ROM protocol is stop-and-wait per 1 KB block, so the rate
// helps only so much.
constexpr int kFlashBauds[] = {2000000, 921600};
constexpr uint32_t kBlock = 0x400;           // the ROM's flash write size
constexpr uint32_t kMainFlashBytes = 8 << 20; // N8R2 / WROOM-1-N8R2

enum Cmd : uint8_t {
    FLASH_BEGIN = 0x02, FLASH_DATA = 0x03, FLASH_END = 0x04, SYNC = 0x08,
    READ_REG = 0x0a, SPI_SET_PARAMS = 0x0b, SPI_ATTACH = 0x0d, CHANGE_BAUDRATE = 0x0f,
    SPI_FLASH_MD5 = 0x13,
};

// All buffers static: the caller is the service task, whose 4 KB stack a
// 1 KB packet plus its 2 KB SLIP encoding overflowed (it corrupted the
// sampler task's stack next to it: a "cache error" panic on both cores).
uint8_t rxBuf[128];
size_t rxLen = 0;
uint32_t rxValue = 0;
uint8_t block[16 + kBlock];               // FLASH_DATA: four header words, then the data
uint8_t pkt[8 + 16 + kBlock];             // one request packet
uint8_t slipOut[2 * sizeof(pkt) + 2];     // its SLIP encoding
uint8_t frame[8 + sizeof(rxBuf)];         // one response frame

void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

void slipWrite(const uint8_t *data, size_t len)
{
    uint8_t *out = slipOut;
    size_t n = 0;
    out[n++] = 0xc0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == 0xc0) { out[n++] = 0xdb; out[n++] = 0xdc; }
        else if (data[i] == 0xdb) { out[n++] = 0xdb; out[n++] = 0xdd; }
        else out[n++] = data[i];
    }
    out[n++] = 0xc0;
    uart_write_bytes(kUart, out, n);
}

// One SLIP frame into frame (up to cap bytes), or -1 on timeout.
int slipRead(uint8_t *frame, size_t cap, int timeoutMs)
{
    const int64_t deadline = esp_timer_get_time() + int64_t(timeoutMs) * 1000;
    bool inFrame = false, escape = false;
    size_t n = 0;
    while (esp_timer_get_time() < deadline) {
        uint8_t c;
        if (uart_read_bytes(kUart, &c, 1, pdMS_TO_TICKS(5)) != 1)
            continue;
        if (!inFrame) {
            if (c == 0xc0) { inFrame = true; n = 0; }
            continue;
        }
        if (c == 0xc0) {
            if (n == 0) continue;   // back-to-back delimiters
            return static_cast<int>(n);
        }
        if (escape) {
            c = c == 0xdc ? 0xc0 : c == 0xdd ? 0xdb : c;
            escape = false;
        } else if (c == 0xdb) {
            escape = true;
            continue;
        }
        if (n < cap) frame[n++] = c;
    }
    return -1;
}

// Send cmd with data; wait for its response. On success rxBuf/rxLen hold
// the response data (status bytes included) and rxValue its value word.
esp_err_t command(uint8_t cmd, const uint8_t *data, size_t len, uint32_t checksum, int timeoutMs)
{
    pkt[0] = 0x00;
    pkt[1] = cmd;
    pkt[2] = len & 0xff;
    pkt[3] = len >> 8;
    put32(pkt + 4, checksum);
    memcpy(pkt + 8, data, len);
    slipWrite(pkt, 8 + len);
    const int64_t deadline = esp_timer_get_time() + int64_t(timeoutMs) * 1000;
    for (;;) {
        const int left = static_cast<int>((deadline - esp_timer_get_time()) / 1000);
        if (left <= 0) return ESP_ERR_TIMEOUT;
        const int n = slipRead(frame, sizeof(frame), left);
        if (n < 0) return ESP_ERR_TIMEOUT;
        if (n < 8 || frame[0] != 0x01 || frame[1] != cmd)
            continue;   // stray: an extra SYNC reply, boot noise
        const size_t size = frame[2] | (frame[3] << 8);
        memcpy(&rxValue, frame + 4, 4);
        rxLen = size < sizeof(rxBuf) ? size : sizeof(rxBuf);
        memcpy(rxBuf, frame + 8, rxLen);
        // The status bytes end the data: 2 (stub) or 4 (ROM). A plain
        // reply is just those; SPI_FLASH_MD5 has the digest first.
        const size_t status = cmd == SPI_FLASH_MD5 ? (rxLen >= 34 ? 32 : 16) : 0;
        if (rxLen < status + 2) return ESP_ERR_INVALID_RESPONSE;
        if (rxBuf[status] != 0) {
            printf("mainFlasher: cmd %02x failed: status %d error %d\n", cmd, rxBuf[status],
                   rxBuf[status + 1]);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
}

esp_err_t sync()
{
    uint8_t data[36] = {0x07, 0x07, 0x12, 0x20};
    memset(data + 4, 0x55, 32);
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (command(SYNC, data, sizeof(data), 0, 100) == ESP_OK) {
            // The ROM answers a SYNC several times: drain the rest.
            vTaskDelay(pdMS_TO_TICKS(50));
            uart_flush_input(kUart);
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t attachFlash()
{
    uint8_t attach[8] = {};   // default SPI pins; the ROM's extra "is legacy" word
    esp_err_t err = command(SPI_ATTACH, attach, sizeof(attach), 0, 3000);
    if (err != ESP_OK) return err;
    uint8_t params[24];
    put32(params + 0, 0);                // flash id
    put32(params + 4, kMainFlashBytes);  // total size
    put32(params + 8, 64 * 1024);        // block
    put32(params + 12, 4 * 1024);        // sector
    put32(params + 16, 256);             // page
    put32(params + 20, 0xffff);          // status mask
    return command(SPI_SET_PARAMS, params, sizeof(params), 0, 3000);
}

esp_err_t changeBaud(int baud)
{
    uint8_t data[8];
    put32(data, baud);
    put32(data + 4, 0);   // the ROM ignores the old rate
    const esp_err_t err = command(CHANGE_BAUDRATE, data, sizeof(data), 0, 3000);
    if (err != ESP_OK) return err;
    uart_wait_tx_done(kUart, pdMS_TO_TICKS(100));
    uart_set_baudrate(kUart, baud);
    vTaskDelay(pdMS_TO_TICKS(50));
    uart_flush_input(kUart);
    // Check the new rate with a quick register read (the chip-detect word
    // esptool reads), so a rate the wires can't carry fails in half a
    // second, not after FLASH_BEGIN's erase-sized timeout.
    uint8_t reg[4];
    put32(reg, 0x40001000);
    return command(READ_REG, reg, sizeof(reg), 0, 500);
}

// MD5 of size bytes from f (rewound first), as 32 lowercase hex digits.
bool fileMd5(FILE *f, uint32_t size, char hex[33])
{
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    rewind(f);
    uint32_t left = size;
    while (left) {
        const size_t want = left < kBlock ? left : kBlock;
        if (fread(block, 1, want, f) != want) return false;
        esp_rom_md5_update(&ctx, block, want);
        left -= want;
    }
    uint8_t digest[16];
    esp_rom_md5_final(digest, &ctx);
    for (int i = 0; i < 16; ++i) sprintf(hex + 2 * i, "%02x", digest[i]);
    return true;
}

esp_err_t flashFile(const Image &image)
{
    FILE *f = fopen(image.path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    struct stat st;
    stat(image.path, &st);
    const uint32_t size = st.st_size;
    const uint32_t blocks = (size + kBlock - 1) / kBlock;
    const int64_t start = esp_timer_get_time();
    printf("mainFlasher: %s: %" PRIu32 " bytes at 0x%06" PRIx32 "\n", image.path, size, image.offset);

    uint8_t begin[20];
    put32(begin + 0, size);           // erase size
    put32(begin + 4, blocks);
    put32(begin + 8, kBlock);
    put32(begin + 12, image.offset);
    put32(begin + 16, 0);             // not encrypted (S2 and later ROMs)
    // The ROM erases the region before answering: allow ~30 s per MB.
    esp_err_t err = command(FLASH_BEGIN, begin, sizeof(begin), 0,
                            3000 + static_cast<int>(size / 1024 * 30));
    if (err != ESP_OK) { fclose(f); return err; }
    const int64_t erased = esp_timer_get_time();

    for (uint32_t seq = 0; seq < blocks; ++seq) {
        uint8_t *data = block + 16;
        const size_t got = fread(data, 1, kBlock, f);
        memset(data + got, 0xff, kBlock - got);   // pad the last block
        uint8_t checksum = 0xef;
        for (uint32_t i = 0; i < kBlock; ++i) checksum ^= data[i];
        put32(block + 0, kBlock);
        put32(block + 4, seq);
        put32(block + 8, 0);
        put32(block + 12, 0);
        for (int attempt = 0; attempt < 3; ++attempt) {
            err = command(FLASH_DATA, block, 16 + kBlock, checksum, 3000);
            if (err != ESP_ERR_TIMEOUT) break;
        }
        if (err != ESP_OK) {
            printf("mainFlasher: block %" PRIu32 " of %" PRIu32 " failed: %s\n", seq, blocks,
                   esp_err_to_name(err));
            fclose(f);
            return err;
        }
    }

    // Verify: the ROM's MD5 of what it wrote against the file's.
    char want[33];
    const bool haveMd5 = fileMd5(f, size, want);
    fclose(f);
    if (!haveMd5) return ESP_FAIL;
    uint8_t md5req[16];
    put32(md5req + 0, image.offset);
    put32(md5req + 4, size);
    put32(md5req + 8, 0);
    put32(md5req + 12, 0);
    err = command(SPI_FLASH_MD5, md5req, sizeof(md5req), 0, 3000 + static_cast<int>(size / 1024 * 8));
    if (err != ESP_OK) return err;
    char got[33] = {};
    if (rxLen >= 34) {
        memcpy(got, rxBuf, 32);   // the ROM: hex digits
    } else {
        for (int i = 0; i < 16; ++i) sprintf(got + 2 * i, "%02x", rxBuf[i]);   // a stub: raw
    }
    const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    const int eraseMs = static_cast<int>((erased - start) / 1000);
    if (strcasecmp(got, want) != 0) {
        printf("mainFlasher: %s: MD5 mismatch: flash %s, file %s\n", image.path, got, want);
        return ESP_ERR_INVALID_CRC;
    }
    printf("mainFlasher: %s: written and verified in %d ms (erase %d ms; %.0f kB/s), md5 %s\n",
           image.path, ms, eraseMs, size / 1.024 / ms, want);
    return ESP_OK;
}

void openDrain(gpio_num_t pin, int level)
{
    gpio_set_level(pin, level);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
}

esp_err_t uartBegin()
{
    if (uart_is_driver_installed(kUart))
        uart_driver_delete(kUart);
    uart_config_t cfg = {};
    cfg.baud_rate = kRomBaud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    esp_err_t err = uart_driver_install(kUart, 4096, 4096, 0, nullptr, 0);
    if (err != ESP_OK) return err;
    err = uart_param_config(kUart, &cfg);
    if (err != ESP_OK) return err;
    return uart_set_pin(kUart, kTxGpio, kRxGpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void uartEnd()
{
    uart_driver_delete(kUart);
    // Back to inputs: TX idles high into the main board's RXD0 otherwise,
    // and that's an Atari data line.
    gpio_reset_pin(static_cast<gpio_num_t>(kTxGpio));
    gpio_reset_pin(static_cast<gpio_num_t>(kRxGpio));
}

// The Sidecar's inputs on Atari bus lines (hardware/tools/sidecar_wiring.py),
// and the flash UART's two wires, which are Atari data lines at the main
// board: GPIO12 -> main RXD0 = D6, GPIO13 <- main TXD0 = D5.
struct BusLine {
    int gpio;
    const char *name;
};
constexpr BusLine kBusLines[] = {
    {4, "phi2"}, {5, "R/W"}, {6, "extSel_"}, {7, "D0"}, {15, "D1"}, {16, "A0"},
    {17, "refresh_"}, {kTxGpio, "D6 (main RXD0)"}, {kRxGpio, "D5 (main TXD0)"},
};

// Edges on the bus lines over ms milliseconds, as a GPIO bitmask. All of
// them are below GPIO32: one register.
uint32_t busActivity(int ms)
{
    uint32_t mask = 0;
    for (const BusLine &l : kBusLines) {
        mask |= 1u << l.gpio;
        gpio_input_enable(static_cast<gpio_num_t>(l.gpio));
    }
    uint32_t changed = 0;
    uint32_t last = REG_READ(GPIO_IN_REG);
    const int64_t end = esp_timer_get_time() + int64_t(ms) * 1000;
    while (esp_timer_get_time() < end) {
        for (int i = 0; i < 256; ++i) {   // fast reads between clock checks
            const uint32_t now = REG_READ(GPIO_IN_REG);
            changed |= now ^ last;
            last = now;
        }
    }
    return changed & mask;
}

void printActivity(const char *when, uint32_t changed)
{
    printf("mainFlasher: bus lines %s:", when);
    if (!changed) {
        printf(" all quiet\n");
        return;
    }
    for (const BusLine &l : kBusLines)
        if (changed & (1u << l.gpio)) printf(" %s", l.name);
    printf(" active\n");
}

// The paranoid check before download mode, with the main board held in
// reset (EN low releases all its pins, and a PBI board in reset is harmless
// even with the Atari on): any edge on a bus line or a UART wire is then an
// Atari running. Released lines with nothing on them would float and read
// as noise (a false "Atari" on the bench), so the check gives each a weak
// internal pull-down: a running Atari drives far harder than ~45 k, an idle
// line then reads a steady low. extSel_ and refresh_ keep the pull-ups the
// sampler gave them, as steady. Download mode would then fail anyway (a powered Atari pulls
// MPD, the main board's GPIO46, high), and the UART would drive D6 and read
// D5 of a live bus. Call with EN held low and the UART not installed.
constexpr int kCheckSettleMs = 20, kCheckMs = 50;
bool busQuietInReset()
{
    for (const BusLine &l : kBusLines)
        if (l.gpio != 6 && l.gpio != 17) gpio_pulldown_en(static_cast<gpio_num_t>(l.gpio));
    vTaskDelay(pdMS_TO_TICKS(kCheckSettleMs));   // the main board's pins let go
    const uint32_t changed = busActivity(kCheckMs);
    for (const BusLine &l : kBusLines)
        if (l.gpio != 6 && l.gpio != 17) gpio_pulldown_dis(static_cast<gpio_num_t>(l.gpio));
    printActivity("with the main board in reset", changed);
    return changed == 0;
}

}  // namespace

void resetMain()
{
    openDrain(kBootGpio, 1);
    openDrain(kEnGpio, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    openDrain(kEnGpio, 1);   // released: the main board's RC brings EN up
    vTaskDelay(pdMS_TO_TICKS(100));
    // Release IO0 (it's the link wire: back to an input for the link).
    gpio_set_direction(kBootGpio, GPIO_MODE_INPUT);
}

esp_err_t enterDownloadMode()
{
    if (uart_is_driver_installed(kUart)) {
        printf("mainFlasher: refusing download mode: the flash UART is already driving its pins\n");
        return ESP_ERR_INVALID_STATE;
    }
    openDrain(kBootGpio, 1);
    openDrain(kEnGpio, 0);   // hold the main board in reset for the check
    if (!busQuietInReset()) {
        printf("mainFlasher: refusing download mode: an Atari is running on the bus "
               "(serial flashing needs the Atari off); main board released to boot\n");
        openDrain(kEnGpio, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_direction(kBootGpio, GPIO_MODE_INPUT);
        return ESP_ERR_INVALID_STATE;
    }
    openDrain(kBootGpio, 0);   // IO0 low across the EN release: download mode
    vTaskDelay(pdMS_TO_TICKS(10));
    openDrain(kEnGpio, 1);
    // The ROM samples IO0 as EN rises; hold it past that.
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_direction(kBootGpio, GPIO_MODE_INPUT);
    return ESP_OK;
}

bool checkBusSelfTest()
{
    // Only on a quiet bus: the test drives the D6 wire itself.
    openDrain(kEnGpio, 0);
    if (!busQuietInReset()) {
        printf("mainFlasher: self-test not run: the bus isn't quiet (an Atari?)\n");
        openDrain(kEnGpio, 1);
        return false;
    }
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_4_BIT;
    timer.timer_num = LEDC_TIMER_3;
    timer.freq_hz = 10000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_channel_config_t ch = {};
    ch.gpio_num = kTxGpio;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = LEDC_CHANNEL_7;
    ch.timer_sel = LEDC_TIMER_3;
    ch.duty = 8;   // 50 %
    const bool started = ledc_timer_config(&timer) == ESP_OK && ledc_channel_config(&ch) == ESP_OK;
    const bool detected = started && !busQuietInReset();
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 0);
    gpio_reset_pin(static_cast<gpio_num_t>(kTxGpio));
    openDrain(kEnGpio, 1);   // let it boot again
    printf("mainFlasher: self-test %s: a 10 kHz square wave on the D6 wire %s\n",
           detected ? "PASSED" : "FAILED", detected ? "was caught" : "went unnoticed");
    return detected;
}

void checkBus()
{
    printActivity("with the main board running", busActivity(kCheckMs));
    openDrain(kEnGpio, 0);
    busQuietInReset();
    openDrain(kEnGpio, 1);   // let it boot again
}

esp_err_t flashImages(const Image *images, size_t count)
{
    size_t present = 0;
    for (size_t i = 0; i < count; ++i) {
        struct stat st;
        if (stat(images[i].path, &st) == 0) ++present;
        else printf("mainFlasher: %s: not found, skipped\n", images[i].path);
    }
    if (present == 0) {
        printf("mainFlasher: nothing to flash\n");
        return ESP_ERR_NOT_FOUND;
    }
    const int64_t start = esp_timer_get_time();
    esp_err_t err = ESP_FAIL;
    for (const int baud : kFlashBauds) {
        // The bus check first, with the UART still off: then download mode.
        err = enterDownloadMode();
        if (err != ESP_OK) return err;
        err = uartBegin();
        if (err != ESP_OK) {
            printf("mainFlasher: UART setup failed: %s\n", esp_err_to_name(err));
            resetMain();
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        uart_flush_input(kUart);
        err = sync();
        if (err == ESP_OK) err = attachFlash();
        if (err != ESP_OK) {
            printf("mainFlasher: no answer from the main board's ROM (%s): check EN/IO0/UART "
                   "wiring, and that the Atari is off (MPD must be low)\n", esp_err_to_name(err));
            uartEnd();
            break;   // a slower rate won't help: the ROM never answered at 115200
        }
        err = changeBaud(baud);
        for (size_t i = 0; i < count && err == ESP_OK; ++i) {
            struct stat st;
            if (stat(images[i].path, &st) != 0) continue;
            err = flashFile(images[i]);
        }
        uartEnd();
        if (err == ESP_OK) break;
        printf("mainFlasher: failed at %d baud (%s)%s\n", baud, esp_err_to_name(err),
               baud != kFlashBauds[1] ? "; again, slower" : "");
    }
    resetMain();
    printf("mainFlasher: %s in %d ms; main board reset into its firmware\n",
           err == ESP_OK ? "done" : esp_err_to_name(err),
           static_cast<int>((esp_timer_get_time() - start) / 1000));
    return err;
}

}  // namespace mainFlasher
