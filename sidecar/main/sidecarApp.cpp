// The Sidecar app: see sidecarApp.h.

#include "sidecarApp.h"

#include "sidecar.h"

#include "esp_log.h"
#include "esp_timer.h"

namespace {

[[maybe_unused]] const char *TAG = "sidecar";   // ESP_LOGI compiles away under CSIM

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

AtariService service;
uint32_t handled = 0;
int64_t nextLog = 0;

}  // namespace

const i2slink_config_t sidecarLinkConfig = {
    .clock = I2SLINK_CLOCK_SOURCE,
    .i2s_port = 0,         // I2S_NUM_0
    .bclk_gpio = 36,
    .tx_gpio = 35,         // to the host's RX
    .rx_gpio = 37,         // from the host's TX
    .rx_bclk_invert = true,
    .sync_timeout_us = 0,  // default: four wire cycles
};

void sidecarAppBegin()
{
    ESP_ERROR_CHECK(service.begin(sidecarLinkConfig));
    nextLog = esp_timer_get_time() + 1000000;
}

void sidecarAppPoll()
{
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
