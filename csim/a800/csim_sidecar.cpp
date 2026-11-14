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

#include <stdio.h>
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

// At exit: each link end's counters (the real i2slink_stats()).
void reportLink(const char *name, i2slink_t *link)
{
    if (!link) {
        return;
    }
    i2slink_stats_t s;
    i2slink_stats(link, &s);
    printf("i2slink %s: writes %u reads new %u timeout %u torn %u crc %u no_lock %u stalled %u | "
           "windows %u waited %u immediate %u late %u tripped %u stale %u torn copies %u skipped %u "
           "jumps %u framing %u locks %u (fallback %u) hints %u peeks %u (torn %u)\n",
           name, (unsigned)s.writes, (unsigned)s.reads_new, (unsigned)s.reads_timeout,
           (unsigned)s.reads_torn, (unsigned)s.reads_crc, (unsigned)s.reads_no_lock,
           (unsigned)s.reads_stalled, (unsigned)s.windows, (unsigned)s.waited,
           (unsigned)s.immediate, (unsigned)s.late, (unsigned)s.tripped, (unsigned)s.stale,
           (unsigned)s.torn, (unsigned)s.skipped, (unsigned)s.jumps, (unsigned)s.framing,
           (unsigned)s.locks, (unsigned)s.locks_fallback, (unsigned)s.hints_true,
           (unsigned)s.peeks, (unsigned)s.peeks_torn);
}

void reportLinks()
{
    reportLink("host", sidecarHost.sidecar().link());
    reportLink("sidecar", sidecarAppLink());
}

struct SidecarSimSetup {
    SidecarSimSetup()
    {
        csim_i2slink_wire(sidecarTxPin, sidecarLinkConfig.rx_gpio);
        csim_i2slink_wire(sidecarLinkConfig.tx_gpio, sidecarRxPin);
        csim_i2slink_wire(sidecarBclkPin, sidecarLinkConfig.bclk_gpio);
        csim_add_background(sidecarRun);
        atexit(reportLinks);
    }
} sidecarSimSetup;

}  // namespace

void csim_sidecar_tick()
{
    sidecarRun();
}
