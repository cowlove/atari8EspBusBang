#pragma once

// The Sidecar's RGB status LED (an SK6812MINI/WS2812 on SIDECAR_LED_GPIO:
// GPIO48 on the Sidecar boards and the DevKitC-1 v1.0; 38 on a v1.1), as an
// activity indicator. Driven by IDF's RMT TX driver with its bytes encoder;
// only the Sidecar's service loop touches it (activityLedPoll()).
//
// At rest it shows the link: dim blue while it's locked onto main's frames,
// dim red when it isn't (or main's frame clock stopped in the last second). Activity flashes over that and fades:
// green a sector read, orange a sector write, cyan any other command
// (mount, status, fetches), magenta a console file transfer.

#ifndef SIDECAR_LED_GPIO
#define SIDECAR_LED_GPIO 48
#endif

enum class LedActivity { Read, Write, Command, Transfer };

#include <cstdint>

#include "i2slink.h"

#ifdef CSIM   // no LED (and no RMT) in the simulator
inline void activityLedBegin() {}
inline void activityLed(LedActivity) {}
inline void activityLedPoll(int64_t, const i2slink_t *) {}
#else
void activityLedBegin();
void activityLed(LedActivity what);   // cheap: just notes it for the next frame
// Call from the service loop; draws a frame every 20 ms (and only then
// reads the link's stats, for its received-frame count).
void activityLedPoll(int64_t nowUs, const i2slink_t *link);
#endif
