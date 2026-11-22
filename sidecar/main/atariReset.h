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
// Released (an input) from begin() until hold() or pulse().
namespace atariReset {
void begin();          // the pin released (an input); call at startup
void hold();           // RESET low: the Atari held in reset
void release();        // RESET released
void pulse(int ms);    // hold for ms, then release
bool held();
}
