#pragma once

// The Sidecar's network: WiFi, the web server, the telnet server, mDNS, and
// the network SMB mounts need. Main has none; it sends its boot profile's
// settings once per boot (NetConfig, sidecar.h).
//
// sidecarNet.cpp and sidecarTelnet.cpp are hardware only; csim links nop
// stubs (csim/a800/csim_sidecar_net.cpp) whose telnet "client" is the
// console.

#include <cstdint>
#include <string>

class SidecarStream;

struct SidecarNetSettings {
    bool wifi = false;
    bool mdns = false;
    uint16_t telnetPort = 0;
    uint16_t connectDelaySec = 0;
    std::string ssid, password, hostname;
};

// False if this build has no network (csim): NetConfig is answered 1.
bool sidecarNetAvailable();

// Main's settings (from the NetConfig handler's copy, in the service task):
// starts the network task (WiFi after connectDelaySec, then the servers).
// The telnet server passes its client through stream (the Telnet channel).
void sidecarNetConfigure(const SidecarNetSettings &settings, SidecarStream &telnet);

// Sio::setNetworkUp()'s hook, for smb: mounts: bring WiFi up now if it isn't
// and wait (a few seconds at most) for an address.
void sidecarNetUp();

// The service loop's pass: the telnet stream's console side (csim; on the
// board the telnet server's own task drains the stream).
void sidecarNetPoll(SidecarStream &telnet);

// Console: the network's state.
void sidecarNetReport();
