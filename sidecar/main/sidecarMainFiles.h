#pragma once
// Main's files, from the Sidecar (the link's FileOpen ... commands,
// components/sidecar_link/sidecar.h), for the console's "main:" filespecs
// and the web pages (sidecarWeb.cpp). Main serves them only in diagnostic
// mode, one open file at a time.
//
// The mainFiles calls drive the link, so they run on the service task:
// console commands are already there; the web server's task hands its
// work over with sidecarRunInServiceLoop(), and holds main's file channel
// across a transfer with claim()/release() (the console then refuses).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sidecar.h"

namespace mainFiles {
constexpr uint32_t kReadChunkBytes = ReadChunkResponse::kDataBytes;    // read()'s buffer
constexpr uint32_t kWriteChunkBytes = WriteChunkCommand::kDataBytes;   // write()'s most

// Main is in diagnostic mode (its last boot report asked for it), else
// *why says how to get there.
bool available(std::string *why);
// Exclusive use across several calls (a web transfer). False: already held.
bool claim();
void release();
bool busy();

// Each returns false with *why (may be nullptr) on failure.
bool open(const char *path, SidecarFileMode mode, uint32_t *size, std::string *why);
bool read(uint32_t offset, uint8_t *buf, uint32_t *length, std::string *why);   // 0 bytes: the end
bool write(uint32_t offset, const uint8_t *data, uint32_t length, std::string *why);
bool close(bool commit, std::string *why);   // Write: commit renames <path>.part to <path>
bool remove(const char *path, std::string *why);
bool hash(const char *path, uint8_t digest[32], uint32_t *size, std::string *why);
struct Entry {
    std::string name;
    long size;   // -1: a directory
};
bool list(const char *dir, std::vector<Entry> *entries, std::string *why);
}  // namespace mainFiles

// Run fn on the service task and wait for it (the board's web server task;
// in csim the web server already runs in the service loop: fn runs at once).
void sidecarRunInServiceLoop(const std::function<void()> &fn);

// A local file's SHA-256 and size (false: can't open it, errno says why).
bool sidecarHashFile(const char *path, uint8_t digest[32], uint32_t *size);
void sidecarHexDigest(const uint8_t digest[32], char hex[65]);
