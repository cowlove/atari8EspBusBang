#pragma once

// The Sidecar's network: WiFi, the web server, the telnet server, mDNS,
// and the network SMB mounts need. Main has no web server, telnet server or
// mDNS of its own. The settings are the boot profile's (main/sysConfig.cpp,
// built into both apps).
//
// sidecarNet.cpp and sidecarTelnet.cpp are hardware only; csim links
// csim/a800/csim_sidecar_net.cpp (no WiFi; the web server for real, on a
// host port; the telnet "client" is the console).

#include <cstdint>
#include <string>

class SidecarStream;

struct SidecarNetSettings {
    bool wifi = false;
    bool mdns = false;
    uint16_t telnetPort = 0;
    uint16_t httpPort = 0;   // 0: no web server
    uint16_t connectDelaySec = 0;
    std::string ssid, password, hostname;
};

// Once, at the Sidecar's boot: starts the network task (WiFi after
// connectDelaySec, then the servers). "$MAC" in the hostname becomes the
// Sidecar's MAC. The telnet server passes its client through telnet (the
// Telnet channel's stream).
void sidecarNetConfigure(const SidecarNetSettings &settings, SidecarStream &telnet);

// Sio::setNetworkUp()'s hook, for smb: mounts: bring WiFi up now if it isn't
// and wait (a few seconds at most) for an address.
void sidecarNetUp();

// The service loop's pass: with no telnet client (none connected, or no
// WiFi), what main sends on a console-opened session ("telnet open") is
// printed here; csim also polls its web server.
void sidecarNetPoll(SidecarStream &telnet);

// Console: the network's state.
void sidecarNetReport();
// WiFi connected (has an address): the rollback's health check (sidecarApp.cpp).
bool sidecarNetConnected();
