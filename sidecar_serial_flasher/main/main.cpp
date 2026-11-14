// sidecar_serial_flasher: the main board's serial flasher, as its own app
// on the Sidecar's ESP32 (see ../CMakeLists.txt). Built and flashed by
// hand, for initializing new boards with the main board disconnected from
// the Atari; never from a script. It does one thing, once per boot:
//   1. mount the spiffs the Sidecar app's flash_spiffs.sh staged
//      (partitions.csv is the Sidecar app's);
//   2. refuse if phi2/D0/D1 show any activity (a running Atari, or a main
//      board running a FAKE_CLOCK build: park that one first);
//   3. reset main into ROM download mode, write the images in
//      /spiffs/main/ that exist, verify each against the ROM's MD5, and
//      reset main into its firmware;
//   4. relay main's UART0 to this console (its boot output, if any).
// To flash again, reset this board. Then flash the Sidecar app back.

#include "mainFlasher.h"

#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

namespace {
const mainFlasher::Image kMainImages[] = {
    {"/spiffs/main/bootloader.bin", 0x0},
    {"/spiffs/main/partition-table.bin", 0x8000},
    {"/spiffs/main/ota_data_initial.bin", 0xe000},
    {"/spiffs/main/app.bin.gz", 0x10000},   // inflated as it is sent
};
constexpr int kMainConsoleBaud = 115200;   // main's UART0 at boot (ROM and IDF default)
}  // namespace

extern "C" void app_main(void)
{
    // Time for a monitor to attach after the reset.
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("sidecar_serial_flasher: serial flashing the main board from /spiffs/main/ "
           "(the main board must be disconnected from the Atari)\n");
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = "/spiffs";
    conf.max_files = 4;
    conf.format_if_mount_failed = false;
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        printf("sidecar_serial_flasher: SPIFFS mount failed (%s): stage main's images with "
               "sidecar/flash_spiffs.sh first\n", esp_err_to_name(err));
    } else {
        mainFlasher::flashImages(kMainImages, sizeof(kMainImages) / sizeof(kMainImages[0]));
    }
    mainFlasher::relayMainConsole(kMainConsoleBaud);
}
