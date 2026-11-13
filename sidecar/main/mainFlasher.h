#pragma once

// The Sidecar's serial flasher for the main board: resets it into ROM
// download mode and writes images through the ESP32-S3 ROM's serial loader
// protocol (what esptool speaks), over UART1. Wiring (docs/sidecar_wiring.pdf):
//   Sidecar GPIO11 -> main EN (open drain: low resets)
//   Sidecar GPIO9  -> main IO0 (the link's TX/RX wire; low at reset = download mode)
//   Sidecar GPIO12 -> main RXD0 (GPIO44), Sidecar GPIO13 <- main TXD0 (GPIO43)
// The main board's UART0 pins are Atari D6/D5 and download mode needs its
// GPIO46 (MPD) low: only with the Atari off.
//
// Blocking (seconds per image), and GPIO9 is the link's: the caller stops
// the link first (sidecarApp.cpp does).

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace mainFlasher {

// One file to write at a flash offset.
struct Image {
    const char *path;
    uint32_t offset;
};

// The Atari-activity check guards every reset and flash: phi2, D0 and D1
// must show no transition at all for 50 ms. Any edge (a running Atari, or a
// main board running a FAKE_CLOCK build) refuses with ESP_ERR_INVALID_STATE
// and leaves the main board untouched.

// Reset the main board into its firmware (a normal boot from flash).
esp_err_t resetMain();

// Reset the main board into ROM download mode (IO0 held low across the EN
// release).
esp_err_t enterDownloadMode();

// Print the check's view of phi2/D0/D1 now, and whether it would refuse.
void checkBus();

// Prove the check: with the bus idle, hold the main board in reset, drive a
// 1 MHz square wave (about an Atari's clock) on the D0 wire (Sidecar GPIO7)
// and confirm it's caught.
bool checkBusSelfTest();

// Reset the main board into download mode (checked: refuses with the bus
// active), write the images that exist
// (missing ones are skipped and reported; at least one must exist), verify
// each against the ROM's MD5 of the written region, then reset it into its
// new firmware. Prints progress.
esp_err_t flashImages(const Image *images, size_t count);

}  // namespace mainFlasher
