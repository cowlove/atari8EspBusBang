// Sidecar firmware entry: runs the Sidecar app (sidecarApp.h) in a
// non-blocking loop on core 1.

#include "sidecarApp.h"

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
    // Core 1, never blocking: the loop polls as fast as it can.
    xTaskCreatePinnedToCore(serviceTask, "sidecar", 4096, nullptr, tskIDLE_PRIORITY + 10,
                            nullptr, 1);
}
