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

// Reset the main board into its firmware (a normal boot from flash).
void resetMain();

// Reset the main board into ROM download mode (IO0 held low across the EN
// release), after two paranoid checks. With the main board running, phi2
// must be still: a clock there is a FAKE_CLOCK build driving the bus itself
// (~1.43 MHz) or an Atari (~1.78 MHz); then refuse without touching it.
// Then, with the main board held in reset, every
// Atari bus line the Sidecar sees and both UART wires (Atari D5/D6 at the
// main board) must stay quiet for 50 ms. Any edge means an Atari is running:
// then refuse (ESP_ERR_INVALID_STATE) and let the main board boot normally.
esp_err_t enterDownloadMode();

// Print bus-line activity with the main board running, then held in reset
// (the check above), then let it boot.
void checkBus();

// Prove the check works: with the main board in reset and the bus quiet,
// drive a square wave on the D6 wire (Sidecar GPIO12) and confirm the
// check reports it. Refuses to drive anything if the bus isn't quiet.
bool checkBusSelfTest();

// Reset the main board into download mode (enterDownloadMode()'s check
// first: refuses with an Atari running), write the images that exist
// (missing ones are skipped and reported; at least one must exist), verify
// each against the ROM's MD5 of the written region, then reset it into its
// new firmware. Prints progress.
esp_err_t flashImages(const Image *images, size_t count);

}  // namespace mainFlasher
