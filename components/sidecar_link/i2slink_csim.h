#pragma once

// CSIM only: controls for the simulated i2slink (i2slink_csim.cpp).
//
// The simulation replaces the I2S hardware at the i2slink.h boundary. Each
// direction is a shared buffer standing in for the TX DMA cycle feeding the
// RX DMA ring: write() builds the real wire cycle (VSYNC, HSYNC markers,
// stamps, CRC) and stores it into the peer's RX ring with a random bit
// shift, and read() locks and decodes it with the real wire-format code
// (i2slink_wire.h). There is no timing model: a write lands at once.

// Declare a wire between two GPIOs (one on each chip). Two ends connect when
// each one's tx_gpio is wired to the other's rx_gpio.
void csim_i2slink_wire(int gpio_a, int gpio_b);

// Called after every i2slink_write() (never re-entered: writes made from
// inside the hook don't call it again). The Sidecar simulation polls its
// service loop here, so a command's response is ready by the time the
// host's write() returns.
void csim_i2slink_set_write_hook(void (*hook)(void));
