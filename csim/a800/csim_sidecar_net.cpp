// csim: the Sidecar's network (sidecar/main/sidecarNet.h) as nop stubs. No
// WiFi, no servers; a telnet session opened from the Sidecar's console
// (CSIM_SIDECAR_CMD="telnet open") prints main's screen text here, so the
// Telnet stream runs end to end.

#include "sidecarNet.h"

#include <cstdio>

#include "sidecar.h"

void sidecarNetConfigure(const SidecarNetSettings &, SidecarStream &)
{
}

void sidecarNetUp()
{
    printf("Sidecar net: no network in csim\n");
}

void sidecarNetPoll(SidecarStream &telnet)
{
    uint8_t buf[128];
    while (const uint32_t n = telnet.read(buf, sizeof(buf))) {
        fwrite(buf, 1, n, stdout);
    }
}

void sidecarNetReport()
{
    printf("Sidecar net: none in csim\n");
}
