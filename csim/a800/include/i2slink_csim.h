#pragma once

// CSIM only: the link's wiring. csim builds the real link code
// (i2slink.c) over an emulated ESP32-S3 I2S peripheral and GDMA
// (csim/a800/csim_i2s.cpp): BCLK domains, descriptor chains walked in
// simulated time, RX length write-back. The two sides run on their own
// coroutines (csim/a800/include/csim_coroutine.h) and yield while they wait.

// Declare a wire between two GPIOs (one on each chip): a TX's data pin to
// the peer RX's, or one BCLK pin to the other's.
void csim_i2slink_wire(int gpio_a, int gpio_b);
