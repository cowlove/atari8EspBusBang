#pragma once
// Brings up the network before an SMB connect, for the SMB images
// (diskStitchSmb.h, diskImageSmb.h). nullptr if the app has no network:
// Sio::setNetworkUp() sets it, and Sio::mount() refuses "smb:" without it.
inline void (*smbNetworkUp)() = nullptr;
