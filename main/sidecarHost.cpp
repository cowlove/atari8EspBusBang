#include "sidecarHost.h"

#if CONFIG_A8BB_SIDECAR_ENABLE
#include "pinDefs.h"

#include <stdio.h>
#include <string.h>

Sidecar sidecar;

void sidecarHostSetup()
{
    if (sidecarTxPin < 0 || sidecarRxPin < 0) {
        printf("Sidecar: host link pins not assigned (pinDefs.h); link disabled\n");
        return;
    }
    const i2slink_config_t config = {
        .clock = I2SLINK_CLOCK_FOLLOWER,
        .i2s_port = 0,
        .bclk_gpio = sidecarBclkPin,
        .tx_gpio = sidecarTxPin,
        .rx_gpio = sidecarRxPin,
        .rx_bclk_invert = true,
        .sync_timeout_us = 0,
    };
    if (sidecar.begin(config) != ESP_OK) {
        printf("Sidecar: link init failed\n");
        return;
    }
    if (!sidecar.probe(100000)) {
        printf("Sidecar: not present (i2slink status %d)\n", (int)sidecar.lastLinkStatus());
        return;
    }
    // Smoke test: one SIO command; the stub handler echoes it.
    SioCommand command = {};
    command.body.DDEVIC = 0x31;
    command.body.DUNIT = 1;
    command.body.DCOMND = 'S';
    SioResponse response;
    const SidecarResult result = sidecar.sio(command, response, 100000);
    printf("Sidecar: present; SIO echo %s (DCOMND=%02x)\n",
           result == SidecarResult::Ok ? "ok" : "FAILED",
           result == SidecarResult::Ok ? response.body.DCOMND : 0);
}
#endif
