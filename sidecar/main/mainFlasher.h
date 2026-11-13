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

// Reset the main board: into ROM download mode (IO0 held low across the EN
// release), or a normal boot from flash.
void resetMain(bool download);

// Reset the main board into download mode, write the images that exist
// (missing ones are skipped and reported; at least one must exist), verify
// each against the ROM's MD5 of the written region, then reset it into its
// new firmware. Prints progress.
esp_err_t flashImages(const Image *images, size_t count);

}  // namespace mainFlasher
