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
    // The host is always the clock source (drives BCLK); the Sidecar
    // follows. So the host drives the same two wires (BCLK, its TX) in both
    // modes, and a wired Sidecar is safe in loopback too.
#if CONFIG_A8BB_SIDECAR_LOOPBACK
    // The link hears itself: RX on the TX pin.
    const i2slink_config_t config = {
        .clock = I2SLINK_CLOCK_SOURCE,
        .i2s_port = 0,
        .bclk_gpio = sidecarBclkPin,
        .tx_gpio = sidecarTxPin,
        .rx_gpio = sidecarTxPin,
        .rx_bclk_invert = true,
        .sync_timeout_us = 0,
        .loopback = true,
    };
#else
    const i2slink_config_t config = {
        .clock = I2SLINK_CLOCK_SOURCE,
        .i2s_port = 0,
        .bclk_gpio = sidecarBclkPin,
        .tx_gpio = sidecarTxPin,
        .rx_gpio = sidecarRxPin,
        .rx_bclk_invert = true,
        .sync_timeout_us = 0,
        .loopback = false,
    };
#endif
    if (sidecar.begin(config) != ESP_OK) {
        printf("Sidecar: link init failed\n");
        return;
    }
#if CONFIG_A8BB_SIDECAR_LOOPBACK
    {
        // Startup self-test: a burst of byte-checked echoes.
        auto index = [](SidecarResult result) { return static_cast<int>(result); };
        unsigned counts[index(SidecarResult::Mismatch) + 1] = {};
        const int checks = 200;
        for (int i = 0; i < checks; ++i) {
            ++counts[index(sidecar.loopbackCheck(20000))];
        }
        printf("Sidecar loopback: %d checks: ok=%u timeout=%u linkfault=%u mismatch=%u "
               "(last link status %d)\n", checks, counts[index(SidecarResult::Ok)],
               counts[index(SidecarResult::Timeout)], counts[index(SidecarResult::LinkFault)],
               counts[index(SidecarResult::Mismatch)], (int)sidecar.lastLinkStatus());
    }
#endif
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
