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

// Register a side's poll (its link end named by its tx_gpio). When the
// other side's read finds nothing new and would wait, the simulation runs
// this poll once (that's what waiting brings on hardware: the peer gets to
// run) and reads again; not while busy() says the side is inside one of its
// own calls (its transact() waiting further up the stack), nor re-entered.
void csim_i2slink_set_poll(int tx_gpio, void (*poll)(void), bool (*busy)(void));
