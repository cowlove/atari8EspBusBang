#pragma once
// Reset the main board into its firmware: a normal boot from flash, nothing
// else. Wiring (docs/sidecar_wiring.pdf): Sidecar GPIO11 -> main EN (open
// drain: low resets), GPIO9 -> main IO0 (the link wire), held released
// (high) across the reset so main boots its app, never download mode.
//
// Unchecked: the Sidecar resets main with an Atari attached (main's boot
// report then decides an OTA update or a core fetch; a running Atari is
// left unserved for the reset). Serial flashing is not here: it's the
// console's "flash" (mainFlasher.h), checked, by hand, with the Atari
// disconnected.
namespace mainReset {
void reset();
}
