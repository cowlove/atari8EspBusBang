// Sidecar firmware entry: runs the Sidecar app (sidecarApp.h) in a
// non-blocking loop on core 1.

#include "atariReset.h"
#include "sidecarApp.h"
#include "../../main/sysConfig.h"
#include "sidecarSampler.h"

#include "i2slink.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void serviceTask(void *)
{
    for (;;) {
        sidecarAppPoll();
    }
}

#ifndef BOOT_CONFIG
#define BOOT_CONFIG ""
#endif

extern "C" void app_main(void)
{
    // The boot profile, as main loads it (main/sysConfig.cpp). csim runs
    // both apps in one process, so there the Sidecar uses main's copy.
    // The Atari held in reset first, until main serves the bus (its BOOT
    // COMPLETE: atariReset.h, sidecarApp.cpp).
    atariReset::begin();
    config.load(BOOT_CONFIG);
    sidecarAppBegin();
    sidecarSamplerBegin();   // core 0: the bus sampler
    // Core 1, never blocking: the loop polls as fast as it can.
    // Its calls keep a command and a response on the stack (SidecarPeer's
    // fetch, probe, ...): room for two link payloads (i2slink.h) on top.
    // 12 KB besides: the drives' I/O runs here too, and an SMB drive's
    // first access connects from inside service() (libsmb2's connect,
    // poll/select and lwIP's recv: ~3.7 KB more), which overflowed the
    // 4 KB this had (.3, 2026-11-22, a COPY on its SMB D4:). Its headroom
    // is in the Sidecar's startup line, and a warning if it gets low
    // (sidecarApp.cpp, serviceStackCheck()).
    xTaskCreatePinnedToCore(serviceTask, "sidecar", 12 * 1024 + 2 * I2SLINK_PAYLOAD_BYTES, nullptr,
                            tskIDLE_PRIORITY + 10,
                            nullptr, 1);
}
