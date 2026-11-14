#pragma once

// CSIM only: controls for the simulated i2slink (i2slink_csim.cpp).
//
// The simulation replaces the I2S hardware at the i2slink.h boundary. Each
// direction is a shared buffer standing in for the TX DMA cycle feeding the
// RX DMA ring: write() builds the real wire cycle (VSYNC, HSYNC markers,
// stamps, CRC) and stores it into the peer's RX ring with a random bit
// shift, and read() locks and decodes it with the real wire-format code
// (i2slink_wire.h). Writes arrive on the wire's schedule (a cycle phase per
// direction, the stamped region's pass, the reader's VSYNC window), and a
// read's waits are real waits in simulated time: each side runs on its own
// coroutine (csim/a800/include/csim_coroutine.h) and yields while it waits.

// Declare a wire between two GPIOs (one on each chip). Two ends connect when
// each one's tx_gpio is wired to the other's rx_gpio.
void csim_i2slink_wire(int gpio_a, int gpio_b);
