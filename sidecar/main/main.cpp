// Sidecar app: answers the host's commands over the Sidecar link.
//
// For now a skeleton: the service loop and stub SIO/CIO handlers. Later the
// Sidecar may also take on video, sound, LEDs and bus monitoring.

#include "sidecar.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sidecar";

// The Sidecar drives the link clock. Pins of the bench Sidecar board (the
// i2sDmaSenderBench SLAVE); the host side is the follower.
static const i2slink_config_t kLinkConfig = {
    .clock = I2SLINK_CLOCK_SOURCE,
    .i2s_port = 0,         // I2S_NUM_0
    .bclk_gpio = 36,
    .tx_gpio = 35,         // to the host's RX
    .rx_gpio = 37,         // from the host's TX
    .rx_bclk_invert = true,
    .sync_timeout_us = 0,  // default: four wire cycles
};

class AtariService : public SidecarService {
protected:
    void handleSio(const SioCommand &command, SioResponse &response) override
    {
        // TODO: perform command.body (AtariDCB) using command.data; put the
        // result status in response.body.DSTATS and any read data in
        // response.data.
        (void)command;
        (void)response;
    }

    void handleCio(const CioCommand &command, CioResponse &response) override
    {
        // TODO: perform command.body (AtariIOCB) using command.data; put
        // the result status in response.body.ICSTA and any read data in
        // response.data.
        (void)command;
        (void)response;
    }
};

static AtariService service;

static void serviceTask(void *)
{
    uint32_t handled = 0;
    int64_t nextLog = esp_timer_get_time() + 1000000;
    for (;;) {
        // Other periodic Sidecar work goes here, alongside service().
        if (service.service()) {
            ++handled;
        }
        const int64_t now = esp_timer_get_time();
        if (now >= nextLog) {
            nextLog = now + 1000000;
            i2slink_stats_t stats;
            i2slink_stats(service.link(), &stats);
            ESP_LOGI(TAG, "1s: commands=%u | locks=%u reads{new=%u timeout=%u torn=%u crc=%u "
                     "nolock=%u stalled=%u}",
                     (unsigned)handled, (unsigned)stats.locks, (unsigned)stats.reads_new,
                     (unsigned)stats.reads_timeout, (unsigned)stats.reads_torn,
                     (unsigned)stats.reads_crc, (unsigned)stats.reads_no_lock,
                     (unsigned)stats.reads_stalled);
            handled = 0;
        }
    }
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(service.begin(kLinkConfig));
    // Core 1, never blocking: the loop polls service() as fast as it can.
    xTaskCreatePinnedToCore(serviceTask, "sidecar", 4096, nullptr, tskIDLE_PRIORITY + 10,
                            nullptr, 1);
}
