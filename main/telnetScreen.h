#pragma once
// Main's half of the Sidecar's telnet server (A8BB_SIDECAR): the
// Sidecar runs the server (sidecar/main/sidecarTelnet.cpp) and passes the
// client through the link's Telnet stream (SidecarChannel::Telnet); main
// reads the Atari's screen and sends it back as text, and types the
// client's keys.

// Attach main's end of the Telnet stream. After sidecarHost.begin().
void telnetScreenBegin();

// Typed keys in, screen text out, and the stream's sends. Cold: from the
// PBI IRQ command (handlePbiRequestCold(), bus detached, interrupts on).
void telnetScreenRun();
