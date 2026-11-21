#pragma once

// The serial flasher for the main board, in the Sidecar app: the console's
// "flash" command (sidecarApp.cpp), deliberate and by hand only, for
// initializing new boards with the main board disconnected from the Atari;
// never from a script or automatically (main updates by OTA). Resets main
// into ROM download mode and writes images through the ESP32-S3 ROM's
// serial loader protocol (what esptool speaks), over UART1. Wiring (docs/sidecar_wiring.pdf):
//   Sidecar GPIO11 -> main EN (open drain: low resets)
//   Sidecar GPIO9  -> main IO0 (the link's TX/RX wire; low at reset = download mode)
//   Sidecar GPIO12 -> main RXD0 (GPIO44), Sidecar GPIO13 <- main TXD0 (GPIO43)
// The main board's UART0 pins are Atari D6/D5 and download mode needs its
// GPIO46 (MPD) low: only with the Atari off.
//
// Blocking (seconds per image).

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

// The flash UART's pins (GPIO12 TX to main's RXD0 = D6, GPIO13 RX from its
// TXD0 = D5): floating inputs, no driver, no pulls. They're Atari data
// lines: the UART drives them only inside a flash. The app calls it at boot
// (the chip's reset state isn't relied on); every flash ends with it too.
void releasePins();

// Main's TXD0 as this board's UART RX pin (GPIO13) sees it, with a weak
// pull-down: how many of 100 samples read high (100: driven high, e.g. the
// ROM's UART idling in download mode; 0: low or not connected).
int probeMainTx();

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
