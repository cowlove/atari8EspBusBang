// CSIM: the Sidecar app, running inside the simulation (make SIDECAR=1).
//
// The host and the Sidecar app share the process. Their links meet in the
// simulated i2slink (components/sidecar_link/i2slink_csim.h), wired here pin
// to pin. The app's outer loop runs cooperatively: one poll after every
// link write (so a command's response is ready when the host's write
// returns) and one per emulated CPU tick (its other periodic work).

#include "i2slink_csim.h"
#include "pinDefs.h"
#include "sidecarApp.h"

namespace {

void sidecarPoll()
{
    static bool begun = false;
    if (!begun) {
        begun = true;   // started by the host's first write, so it finds the host's link
        sidecarAppBegin();
    }
    sidecarAppPoll();
}

struct SidecarSimSetup {
    SidecarSimSetup()
    {
        csim_i2slink_wire(sidecarTxPin, sidecarLinkConfig.rx_gpio);
        csim_i2slink_wire(sidecarLinkConfig.tx_gpio, sidecarRxPin);
        csim_i2slink_wire(sidecarBclkPin, sidecarLinkConfig.bclk_gpio);
        csim_i2slink_set_write_hook(sidecarPoll);
    }
} sidecarSimSetup;

}  // namespace

void csim_sidecar_tick()
{
    sidecarPoll();
}
