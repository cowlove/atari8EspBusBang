// CSIM: the Sidecar app, running inside the simulation (make SIDECAR=1).
//
// The host and the Sidecar app share the process. Their links meet in the
// simulated i2slink (components/sidecar_link/i2slink_csim.h), wired here pin
// to pin. Both sides run cooperatively: when one side's read finds nothing
// new and would wait, the simulated link runs the other side's poll (the
// Sidecar's outer loop, or the host's Sidecar::poll()), unless that side is
// inside one of its own link calls; and the Sidecar's loop also runs once
// per emulated CPU tick (its other periodic work).

#include "i2slink_csim.h"
#include "pinDefs.h"
#include "sidecarApp.h"
#include "sidecarHost.h"

#include <stdlib.h>

namespace {

void sidecarPoll()
{
    static bool begun = false;
    if (!begun) {
        begun = true;   // started by the host's first wait, so it finds the host's link
        sidecarAppBegin();
    }
    sidecarAppPoll();
}

bool sidecarBusy()
{
    return sidecarAppBusy();
}

void hostPoll()
{
    sidecarHost.poll();
}

bool hostBusy()
{
    return sidecarHost.sidecar().busy();
}

struct SidecarSimSetup {
    SidecarSimSetup()
    {
        csim_i2slink_wire(sidecarTxPin, sidecarLinkConfig.rx_gpio);
        csim_i2slink_wire(sidecarLinkConfig.tx_gpio, sidecarRxPin);
        csim_i2slink_wire(sidecarBclkPin, sidecarLinkConfig.bclk_gpio);
        csim_i2slink_set_poll(sidecarLinkConfig.tx_gpio, sidecarPoll, sidecarBusy);
        csim_i2slink_set_poll(sidecarTxPin, hostPoll, hostBusy);
    }
} sidecarSimSetup;

}  // namespace

void csim_sidecar_tick()
{
    sidecarPoll();
    // CSIM_SIDECAR_CMD: one Sidecar console command, run once the host is
    // up (it has probed the Sidecar) and from here, where the host isn't
    // inside a link call of its own: a Sidecar command needs the host's
    // poll to answer it.
    static bool cmdDone = false;
    if (!cmdDone && sidecarHost.sidecar().present() && !hostBusy()) {
        cmdDone = true;
        if (const char *cmd = getenv("CSIM_SIDECAR_CMD")) sidecarAppCommand(cmd);
    }
}
