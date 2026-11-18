#pragma once
// DISK_IMAGE_SMB: the SMB images (smb: diskSpecs). 0 in main's project (the
// top-level CMakeLists.txt): main has no network, so no SMB code is built
// there and Sio::mount() refuses smb:; the Sidecar app (and csim) keep it.
#ifndef DISK_IMAGE_SMB
#define DISK_IMAGE_SMB 1
#endif

// Brings up the network before an SMB connect, for the SMB images
// (diskStitchSmb.h, diskImageSmb.h). nullptr if the app has no network:
// Sio::setNetworkUp() sets it, and Sio::mount() refuses "smb:" without it.
inline void (*smbNetworkUp)() = nullptr;
