#pragma once
// The Sidecar's telnet server (sidecarTelnet.cpp, hardware only): an
// RFC 2217 server whose client is passed through to main over the link's
// Telnet stream; main sends the Atari's screen back as text
// (main/telnetScreen.cpp).

#include <cstdint>

class SidecarStream;

// Start the server on port (once WiFi is up). Its client's bytes go into
// stream (stream.open() on connect, close() on disconnect); what main sends
// goes to the client, or to this console when no client is connected (a
// session opened from the console: "telnet open").
void sidecarTelnetStart(SidecarStream &stream, uint16_t port);
bool sidecarTelnetRunning();
void sidecarTelnetReport();
