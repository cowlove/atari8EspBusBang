#pragma once
// SidecarFileServer: one side's files for the other, over the link's file
// commands (FileOpen, ReadChunk/ReadVersion of the File object,
// WriteChunk, FileClose, FileRemove, FileHash: sidecar.h). One file open
// at a time; all the policy (paths, retries, what to copy) is the client's.
// Main serves its flash to the Sidecar in diagnostic mode; the Sidecar
// serves its storage to main (e.g. a cartridge image at boot).
//
// serve() takes the command in message, a copy that becomes the response,
// and returns true if it was a file command (false: not its; the caller's
// other answers follow). Cold paths only (stdio).

#include <cstdint>
#include <cstdio>
#include <string>

#include "sidecar.h"

class SidecarFileServer {
public:
    // Paths as the client sent them go through resolver first (nullptr: as
    // given), e.g. the Sidecar's Sio::resolve().
    using Resolver = std::string (*)(const std::string &path);
    void setResolver(Resolver resolver) { resolver_ = resolver; }

    bool serve(SidecarCommand command, void *message);

private:
    void abandon();
    std::string resolve(const char *path) const;

    Resolver resolver_ = nullptr;
    FILE *file = nullptr;
    SidecarFileMode mode = SidecarFileMode::Read;
    std::string path;          // as opened, resolved (Write: the final name; the data goes to path.part)
    std::string listing;       // List mode: the directory listing, the "file"
    uint32_t size = 0;         // Read: the file's size
    uint32_t nextOffset = 0;   // Write: the next byte expected
    uint32_t lastOffset = 0, lastLength = 0;   // Write: the last chunk (a retry of it is acknowledged)
    bool open = false;
};
