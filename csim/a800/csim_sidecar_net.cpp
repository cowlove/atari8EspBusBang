// csim: the Sidecar's network (sidecar/main/sidecarNet.h). No WiFi; the
// web server is real, on a host port (csim_httpd.cpp, config.httpServerPort:
// 8080 in csim, CSIM_HTTP_PORT=<n> to change it, 0 for none), serving the
// Sidecar's pages (sidecar/main/sidecarWeb.cpp). No telnet server: a
// session opened from the Sidecar's console (CSIM_SIDECAR_CMD="telnet
// open") prints main's screen text here, so the Telnet stream runs end to
// end.

#include "sidecarNet.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include "sidecar.h"
#include "sidecarWeb.h"

namespace {
bool webUp = false;
uint16_t webPort = 0;
}  // namespace

void sidecarNetConfigure(const SidecarNetSettings &settings, SidecarStream &)
{
    if (webUp || settings.httpPort == 0) {
        return;
    }
    // The file manager's storage (sidecarWeb.cpp): CSIM_WEB_ROOT, ./webfs by
    // default, with the board's two areas in it.
    const char *root = getenv("CSIM_WEB_ROOT");
    const std::string base = root ? root : "./webfs";
    mkdir(base.c_str(), 0755);
    mkdir((base + "/sd").c_str(), 0755);
    mkdir((base + "/spiffs").c_str(), 0755);
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

void sidecarNetPoll(SidecarStream &telnet)
{
    csimHttpdPoll();
    uint8_t buf[128];
    while (const uint32_t n = telnet.read(buf, sizeof(buf))) {
        fwrite(buf, 1, n, stdout);
    }
}

void sidecarNetReport()
{
    if (webUp)
        printf("Sidecar net: csim web server on port %u, %u requests\n", (unsigned)webPort,
               (unsigned)sidecarWebRequests());
    else
        printf("Sidecar net: none in csim (web server off)\n");
}
