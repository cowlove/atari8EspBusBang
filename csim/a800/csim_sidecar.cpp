// CSIM: the Sidecar app, running inside the simulation.
//
// The host and the Sidecar app share the process. Their links meet in the
// simulated i2slink (components/sidecar_link/i2slink_csim.h), wired here pin
// to pin. The Sidecar runs its real outer loop, as on its own chip
// (sidecar/main/main.cpp: sidecarAppBegin(), then sidecarAppPoll() for
// ever), on a coroutine of its own (csim_coroutine.h): every emulated CPU
// tick resumes it, and so does each step of a host wait on the scheduler's
// stack (setup()'s probe, default mode's in-line core 0 work). It yields
// whenever it waits: in a link read, on every few clock reads, and once a
// loop pass. Neither side is ever called back from inside its own waits.
//
// As on hardware, the host serves Sidecar->main commands only in diagnostic
// mode, so a Sidecar command to the host (a "fetch test" from main, for
// one) times out here.

#include "csim_coroutine.h"
#include "i2slink_csim.h"
#include "pinDefs.h"
#include "sidecarApp.h"
#include "sidecarHost.h"

#include <stdlib.h>

namespace {

void sidecarMain();

// Yield on every 4th clock read: a few loop passes per CPU tick.
CsimCoroutine sidecar("sidecar", sidecarMain, 4);

void sidecarMain()
{
    sidecarAppBegin();
    bool cmdDone = false;
    for (;;) {
        sidecarAppPoll();
        // CSIM_SIDECAR_CMD: one Sidecar console command, run once the host
        // is up (it has probed the Sidecar). Commands that ask the host
        // something time out, as on hardware outside diagnostic mode.
        if (!cmdDone && sidecarHost.sidecar().present()) {
            cmdDone = true;
            if (const char *cmd = getenv("CSIM_SIDECAR_CMD")) {
                sidecarAppCommand(cmd);
            }
        }
        sidecar.clockRead();   // a pass that reads no clock still yields
    }
}

void sidecarRun()
{
    sidecar.resume();
}

struct SidecarSimSetup {
    SidecarSimSetup()
    {
        csim_i2slink_wire(sidecarTxPin, sidecarLinkConfig.rx_gpio);
        csim_i2slink_wire(sidecarLinkConfig.tx_gpio, sidecarRxPin);
        csim_i2slink_wire(sidecarBclkPin, sidecarLinkConfig.bclk_gpio);
        csim_add_background(sidecarRun);
    }
} sidecarSimSetup;

}  // namespace

void csim_sidecar_tick()
{
    sidecarRun();
}
