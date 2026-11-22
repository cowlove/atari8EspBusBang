#pragma once
// The Atari's RESET line, from the Sidecar's GPIO3: open drain (low holds
// the Atari in reset; released, the Atari's own pull-up brings it high).
//
// Wiring: on the DevKitC carrier (both orders, tags carrier-fab-2026-11-12
// and -18) GPIO3 is only on the J7 breakout (pin 5, SC_IO3): a bodge wire
// from J7 pin 5 to J9 pin 1 (RESET_DRV, R1's ESP end), J9 itself left open
// (fitted, it joins main's GPIO19, its USB D-, to the same net). On the SMD
// XE board it's R22, laid out. Unwired, the pin drives nothing: harmless.
//
// The policy (sidecarApp.cpp): held from the Sidecar's start (app_main's
// first call), and again whenever main (re)boots (mainReset::reset(), main's
// boot report); released when main reports BOOT COMPLETE (SystemControl's
// BootComplete flag: setup done, core 1 serving the bus). A diagnostic-mode
// boot never gets there: the Atari stays held. The console's "atari"
// overrides it by hand.
namespace atariReset {
void begin();          // configure the pin and hold RESET: call first thing at startup
void hold();           // RESET low: the Atari held in reset
void release();        // RESET released
void pulse(int ms);    // hold for ms, then release
bool held();
}
