// Sidecar firmware entry: runs the Sidecar app (sidecarApp.h) in a
// non-blocking loop on core 1.

#include "sidecarApp.h"
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

extern "C" void app_main(void)
{
    sidecarAppBegin();
    sidecarSamplerBegin();   // core 0: the bus sampler
    // Core 1, never blocking: the loop polls as fast as it can.
    // Its calls keep a command and a response on the stack (SidecarPeer's
    // fetch, probe, ...): room for two link payloads (i2slink.h) on top.
    xTaskCreatePinnedToCore(serviceTask, "sidecar", 4096 + 2 * I2SLINK_PAYLOAD_BYTES, nullptr,
                            tskIDLE_PRIORITY + 10,
                            nullptr, 1);
}
