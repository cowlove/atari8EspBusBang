// SidecarFileServer: see sidecar_file_server.h. Plain stdio and dirent on
// VFS paths (through the app's resolver, if it set one).

#ifdef CSIM
#include "csim.h"
#endif

#include "sidecar_file_server.h"
#include "sidecar_sha256.h"   // FileHash

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

std::string SidecarFileServer::resolve(const char *path) const
{
    return resolver_ ? resolver_(path) : std::string(path);
}

void SidecarFileServer::abandon()
{
    if (file) fclose(file);
    file = nullptr;
    if (open && mode == SidecarFileMode::Write) remove((path + ".part").c_str());
    listing.clear();
    listing.shrink_to_fit();
    open = false;
}

// The path in data[], NUL-terminated within it, or nullptr.
template <typename Message> static const char *pathOf(Message &m)
{
    return memchr(m.data, 0, sizeof(m.data)) ? reinterpret_cast<const char *>(m.data) : nullptr;
}

template <typename Message> static void status(Message &m, SidecarFileStatus st, int err = 0)
{
    m.header.status = static_cast<uint8_t>(st);
    m.body.err = err;
}

bool SidecarFileServer::serve(SidecarCommand command, void *message)
{
    switch (command) {
    case SidecarCommand::FileOpen: {
        auto &m = *static_cast<FileOpenResponse *>(message);
        const char *p = pathOf(m);
        abandon();
        m.body.size = 0;
        if (!p || m.body.mode > static_cast<uint8_t>(SidecarFileMode::List)) {
            status(m, SidecarFileStatus::BadRequest);
            return true;
        }
        mode = static_cast<SidecarFileMode>(m.body.mode);
        path = resolve(p);
        if (mode == SidecarFileMode::List) {
            DIR *dir = opendir(path.c_str());
            if (!dir) {
                status(m, SidecarFileStatus::Failed, errno);
                return true;
            }
            while (const dirent *e = readdir(dir)) {
                struct stat st;
                const std::string full = path + (path.empty() || path.back() != '/' ? "/" : "") + e->d_name;
                const bool isDir = e->d_type == DT_DIR;
                const long sz = isDir ? -1 : (stat(full.c_str(), &st) == 0 ? (long)st.st_size : -2);
                char line[300];
                snprintf(line, sizeof(line), "%ld %s\n", sz, e->d_name);
                listing += line;
            }
            closedir(dir);
            size = listing.size();
        } else {
            file = fopen(mode == SidecarFileMode::Write ? (path + ".part").c_str() : path.c_str(),
                         mode == SidecarFileMode::Write ? "wb" : "rb");
            if (!file) {
                status(m, SidecarFileStatus::Failed, errno);
                return true;
            }
            size = 0;
            if (mode == SidecarFileMode::Read) {
                fseek(file, 0, SEEK_END);
                size = (uint32_t)ftell(file);
            }
            nextOffset = lastOffset = lastLength = 0;
        }
        open = true;
        m.body.size = size;
        status(m, SidecarFileStatus::Ok);
        return true;
    }
    case SidecarCommand::ReadVersion:
    case SidecarCommand::ReadChunk: {
        auto &chunk = *static_cast<ReadChunkResponse *>(message);
        if (chunk.body.object != static_cast<uint8_t>(SidecarObject::File)) {
            return false;   // not ours (Test, ...)
        }
        if (!open || mode == SidecarFileMode::Write) {
            chunk.header.status = static_cast<uint8_t>(SidecarReadStatus::Unavailable);
            return true;
        }
        if (command == SidecarCommand::ReadVersion) {
            auto &v = *static_cast<ReadVersionResponse *>(message);
            v.body.size = size;
            v.body.version = size;   // no content token: one transfer, then FileHash
            v.header.status = static_cast<uint8_t>(SidecarReadStatus::Ok);
            return true;
        }
        const uint32_t offset = chunk.body.offset;
        const uint32_t left = offset < size ? size - offset : 0;
        uint32_t length = left < sizeof(chunk.data) ? left : sizeof(chunk.data);
        if (length && mode == SidecarFileMode::List) {
            memcpy(chunk.data, listing.data() + offset, length);
        } else if (length && (fseek(file, offset, SEEK_SET) != 0 || fread(chunk.data, 1, length, file) != length)) {
            chunk.header.status = static_cast<uint8_t>(SidecarReadStatus::IoError);
            return true;
        }
        chunk.body.length = static_cast<uint16_t>(length);
        chunk.header.status = static_cast<uint8_t>(SidecarReadStatus::Ok);
        return true;
    }
    case SidecarCommand::WriteChunk: {
        auto &m = *static_cast<WriteChunkResponse *>(message);
        if (!open || mode != SidecarFileMode::Write) {
            status(m, open ? SidecarFileStatus::BadRequest : SidecarFileStatus::Unavailable);
            return true;
        }
        const uint32_t offset = m.body.offset, length = m.body.length;
        if (length > sizeof(m.data)) {
            status(m, SidecarFileStatus::BadRequest);
            return true;
        }
        if (offset == lastOffset && length == lastLength && offset + length == nextOffset && length) {
            status(m, SidecarFileStatus::Ok);   // a retry of the last chunk: already written
            return true;
        }
        if (offset != nextOffset) {
            status(m, SidecarFileStatus::OutOfOrder);
            return true;
        }
        if (fwrite(m.data, 1, length, file) != length) {
            status(m, SidecarFileStatus::Failed, errno);
            return true;
        }
        lastOffset = offset;
        lastLength = length;
        nextOffset = offset + length;
        status(m, SidecarFileStatus::Ok);
        return true;
    }
    case SidecarCommand::FileClose: {
        auto &m = *static_cast<FileCloseResponse *>(message);
        if (!open) {
            status(m, SidecarFileStatus::Unavailable);
            return true;
        }
        int err = 0;
        if (file && fclose(file) != 0) err = errno;
        file = nullptr;
        if (mode == SidecarFileMode::Write) {
            const std::string part = path + ".part";
            if (m.body.commit && !err) {
                remove(path.c_str());   // SPIFFS rename won't replace
                if (rename(part.c_str(), path.c_str()) != 0) err = errno;
            } else {
                remove(part.c_str());
            }
        }
        open = false;
        listing.clear();
        listing.shrink_to_fit();
        status(m, err ? SidecarFileStatus::Failed : SidecarFileStatus::Ok, err);
        return true;
    }
    case SidecarCommand::FileRemove: {
        auto &m = *static_cast<FilePathResponse *>(message);
        const char *p = pathOf(m);
        if (!p) {
            status(m, SidecarFileStatus::BadRequest);
        } else if (remove(resolve(p).c_str()) != 0) {
            status(m, SidecarFileStatus::Failed, errno);
        } else {
            status(m, SidecarFileStatus::Ok);
        }
        return true;
    }
    case SidecarCommand::FileHash: {
        auto &m = *static_cast<FileHashResponse *>(message);
        const char *p = pathOf(m);
        memset(m.body.sha256, 0, sizeof(m.body.sha256));
        m.body.size = 0;
        if (!p) {
            status(m, SidecarFileStatus::BadRequest);
            return true;
        }
        FILE *f = fopen(resolve(p).c_str(), "rb");
        if (!f) {
            status(m, SidecarFileStatus::Failed, errno);
            return true;
        }
        SidecarSha256 sha;
        constexpr size_t kBuf = 2048;   // heap, for the call: no static RAM on main
        uint8_t *buf = static_cast<uint8_t *>(malloc(kBuf));
        if (!buf) {
            fclose(f);
            status(m, SidecarFileStatus::Failed, ENOMEM);
            return true;
        }
        size_t n;
        uint32_t total = 0;
        while ((n = fread(buf, 1, kBuf, f)) > 0) {
            sha.update(buf, n);
            total += n;
        }
        const int err = ferror(f) ? errno : 0;
        fclose(f);
        free(buf);
        sha.finish(m.body.sha256);
        m.body.size = total;
        status(m, err ? SidecarFileStatus::Failed : SidecarFileStatus::Ok, err);
        return true;
    }
    default:
        return false;
    }
}
