#pragma once

// The Sidecar's network: WiFi, the web server, mDNS, and the network SMB
// mounts need. The settings are the boot profile's (main/sysConfig.cpp,
// built into both apps).
//
// sidecarNet.cpp is hardware only; csim links csim/a800/csim_sidecar_net.cpp
// (no WiFi; the web server for real, on a host port).

#include <cstdint>
#include <string>

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
// Sidecar's MAC.
void sidecarNetConfigure(const SidecarNetSettings &settings);

// Sio::setNetworkUp()'s hook, for smb: mounts: bring WiFi up now if it isn't
// and wait (a few seconds at most) for an address.
void sidecarNetUp();

// The service loop's pass (csim: the web server's; nothing on the board,
// where the servers have tasks of their own).
void sidecarNetPoll();

// Console: the network's state.
void sidecarNetReport();
