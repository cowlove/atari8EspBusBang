// csim: the Sidecar's network (sidecar/main/sidecarNet.h). No WiFi; the
// web server is real, on a host port (csim_httpd.cpp, config.httpServerPort:
// 8080 in csim, CSIM_HTTP_PORT=<n> to change it, 0 for none), serving the
// Sidecar's pages (sidecar/main/sidecarWeb.cpp).

#include "sidecarNet.h"

#include <cstdio>

#include "sidecarWeb.h"

namespace {
bool webUp = false;
uint16_t webPort = 0;
}  // namespace

void sidecarNetConfigure(const SidecarNetSettings &settings)
{
    if (webUp || settings.httpPort == 0) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = settings.httpPort;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        printf("Sidecar net: csim web server not started (port %u)\n", (unsigned)settings.httpPort);
        return;
    }
    sidecarWebRegister(server);
    webUp = true;
    webPort = settings.httpPort;
    printf("Sidecar net: csim web server on http://localhost:%u/\n", (unsigned)webPort);
}

void sidecarNetUp()
{
    printf("Sidecar net: no network in csim\n");
}

void sidecarNetPoll()
{
    csimHttpdPoll();
}

void sidecarNetReport()
{
    if (webUp)
        printf("Sidecar net: csim web server on port %u, %u requests\n", (unsigned)webPort,
               (unsigned)sidecarWebRequests());
    else
        printf("Sidecar net: none in csim (web server off)\n");
}
