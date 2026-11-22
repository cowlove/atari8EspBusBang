// CSIM: the Sidecar's two reset lines, the Atari's RESET (atariReset.h) and
// main's EN (mainReset.h), for the Sidecar app csim runs. Its policy code
// (sidecarApp.cpp) is the board's, unchanged: the Atari held from the
// Sidecar's start and at every boot report, released at main's BOOT
// COMPLETE, and the console's "atari hold|release|reset [<ms>]" and
// "reset" (CSIM_SIDECAR_CMD, e.g. "@20 atari reset;@40 reset").
//
// The Atari's RESET: held, bus-cycle mode's 6502 runs no instruction
// (atari800's cpu.c, as for halt_); released, atari800 runs its own warm
// start at the next frame (atari.c, as its RESET key does): PBI, PIA,
// ANTIC and CPU reset, the reset vector. CSIM_RESET_COLD=1: a cold start
// instead (atari800's power-on: COLDST, the cartridge's power-up state).
// Unlike an XL, the chips reset when the line is released, not when it goes
// low (the screen doesn't blank while held), up to a frame (~20 ms) late;
// the 6502 stays stalled until then.
// CSIM_ATARI_RESET=0: no line (never held, as before 2026-11-22).
//
// Main's EN: a coarse reboot. The Sidecar's reset holds the Atari, then the
// scheduler (csim_atari800.cpp, between CPU ticks) abandons core 0's
// coroutine (its stack's frames dropped, destructors unrun), closes main's
// link end and runs setup() again: main's globals and heap carry over (no
// .bss/.data re-init, allocations leak), the boot profile is the same. Bus-
// cycle mode only (the default mode has no core 0 coroutine to restart).
#include "csim.h"
#include "csim_atari800.h"
#include "csim_coroutine.h"

#include "atariReset.h"
#include "mainReset.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
bool enabled()
{
    static const bool on = [] {
        const char *e = getenv("CSIM_ATARI_RESET");
        return !(e && strcmp(e, "0") == 0);
    }();
    return on;
}

bool cold()
{
    static const bool on = [] {
        const char *e = getenv("CSIM_RESET_COLD");
        return e && strcmp(e, "1") == 0;
    }();
    return on;
}

double nowSec() { return CSIM_PEEK_CCOUNT64() / 240e6; }

bool isHeld = false;
int request = 0;               // for atari800's next frame: 1 warm, 2 cold
bool mainRebootPending = false;
}  // namespace

namespace atariReset {
void begin()
{
    hold();   // main isn't serving the bus yet
}

void hold()
{
    if (!enabled() || isHeld)
        return;
    isHeld = true;
    request = 0;   // a release not yet run is superseded
    printf("csim: Atari RESET held at %.3f s\n", nowSec());
}

void release()
{
    if (!isHeld)
        return;
    isHeld = false;
    request = cold() ? 2 : 1;
    printf("csim: Atari RESET released at %.3f s (%s start)\n", nowSec(), cold() ? "cold" : "warm");
}

void pulse(int ms)
{
    if (!enabled())
        return;
    hold();
    csim_wait_until(CSIM_PEEK_CCOUNT64() + uint64_t(ms > 0 ? ms : 1) * 240000ULL);
    release();
}

bool held()
{
    return isHeld;
}
}  // namespace atariReset

namespace mainReset {
void reset()
{
    // As on the board: the Atari waits in reset until main is serving the
    // bus again (its BOOT COMPLETE).
    atariReset::hold();
    mainRebootPending = true;
    printf("csim: main reset (EN) at %.3f s: setup() again at the next CPU tick\n", nowSec());
}
}  // namespace mainReset

// The scheduler's question (csim_atari800.cpp): a main reboot to run now?
bool csim_main_reboot_take()
{
    const bool r = mainRebootPending;
    mainRebootPending = false;
    return r;
}

// Held, or released with the reset not yet run: the 6502 stays stalled
// until atari800's frame takes the request (csim_board_reset_request()),
// so it never runs on from where it stopped first (after a main reboot,
// over memory main no longer serves the same).
extern "C" int csim_board_reset_held(void)
{
    return isHeld || request != 0;
}

extern "C" int csim_board_reset_request(void)
{
    const int r = request;
    request = 0;
    return r;
}
