#pragma once
// DISK_IMAGE_SMB: the SMB images (smb: diskSpecs) are built. The default is
// 1 (the Sidecar app, csim). Main's project defines DISK_IMAGE_SMB_KCONFIG
// (the top-level CMakeLists.txt), so it follows main's Kconfig A8BB_WIFI:
// without WiFi no SMB code is built, libsmb2 drops out of the link and
// Sio::mount() refuses smb: (use sidecar!smb:).
#ifndef DISK_IMAGE_SMB
#ifdef DISK_IMAGE_SMB_KCONFIG
#include "sdkconfig.h"
#if CONFIG_A8BB_WIFI
#define DISK_IMAGE_SMB 1
#else
#define DISK_IMAGE_SMB 0
#endif
#else
#define DISK_IMAGE_SMB 1
#endif
#endif

// Brings up the network before an SMB connect, for the SMB images
// (diskStitchSmb.h, diskImageSmb.h). nullptr if the app has no network:
// Sio::setNetworkUp() sets it, and Sio::mount() refuses "smb:" without it.
inline void (*smbNetworkUp)() = nullptr;
