#pragma once
#include "a8log.h"
#include "diskStitch.h"
#include "smbNetwork.h"

#define SMB
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"

#include <algorithm>
#include <chrono>

// One SMB share (smb://server/share[/dir]) behind a stitched disk.
//
// Design decision (2026-11-24): a connection belongs to the one stitched disk
// that created it and is used only from the thread that serves that disk
// (the Sidecar's service task): no other code (the web server, the console)
// shares it, so it takes no lock. Keep it that way, or add one.
//
// Recovery. libsmb2 drops a request that times out (SMB2_STATUS_IO_TIMEOUT)
// but keeps the context and its socket, which can then fail every later
// request (seen on the test pool: one IO_TIMEOUT, then every open failing
// with EINPROGRESS for the rest of the boot). So when an operation fails,
// recover() asks the server for the base directory's attributes (a stat;
// not smb2_echo(), whose connected check is inverted in this libsmb2 and
// always fails): if it answers, the error was the
// file's own (not found, access) and stands; if not, the connection is torn
// down and made again, and the operation is retried once (the cache opens
// its file again). If the server can't be reached, operations fail fast
// until a backoff (2 s, doubling to 30 s) has passed, rather than each
// waiting out the timeout; the first operation after it tries again.
class SmbConnection : public CachedStorageInterface {
    struct smb2_context *smb2 = NULL;
    struct smb2_url *url = NULL;
    string lastFile;
    string password, urlString;
    string base;              // the URL's directory within the share ("" for its root)
    bool connected = false;
    int64_t retryAtMs = 0;    // connected == false: no new try before this
    int backoffMs = 0;
    uint32_t reconnects = 0, fastFails = 0;

    static constexpr int kTimeoutSec = 3;
    static constexpr int kBackoffFirstMs = 2000, kBackoffMaxMs = 30000;

    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // path within the share: under the URL's directory, if it named one.
    string full(const char *path) const {
        if (base.empty()) return path;
        return *path ? base + "/" + path : base;
    }

    void teardown() {
        if (url) smb2_destroy_url(url);
        if (smb2) smb2_destroy_context(smb2);   // closes the socket
        url = NULL;
        smb2 = NULL;
        connected = false;
    }

    // Connect (again): true if the share is mounted.
    bool open_() {
        teardown();
        smb2 = smb2_init_context();
        if (smb2 == NULL) {
            LOG("SMB: smb2_init_context failed\n");
            return false;
        }
        if (password != "")
            smb2_set_password(smb2, password.c_str());
        url = smb2_parse_url(smb2, urlString.c_str());
        if (url == NULL) {
            LOG("SMB: bad URL '%s': %s\n", urlString.c_str(), smb2_get_error(smb2));
            return false;
        }
        base = url->path ? url->path : "";
        while (!base.empty() && base.back() == '/') base.pop_back();
        smb2_set_timeout(smb2, kTimeoutSec);
        if (smb2_connect_share(smb2, url->server, url->share, url->user) < 0) {
            LOG("smb2_connect_share failed. %s\n", smb2_get_error(smb2));
            return false;
        }
        connected = true;
        return true;
    }

    // Reconnect after a failure; on failure, back off.
    bool reconnect(const char *why) {
        cacheForgetAll();   // their handles die with the context
        const bool ok = open_();
        if (ok) {
            ++reconnects;
            backoffMs = 0;
            LOG("SMB: %s: reconnected to %s (reconnect %u)\n", why, urlString.c_str(), (unsigned)reconnects);
        } else {
            backoffMs = backoffMs ? std::min(backoffMs * 2, kBackoffMaxMs) : kBackoffFirstMs;
            retryAtMs = nowMs() + backoffMs;
            LOG("SMB: %s: reconnecting to %s failed; next try in %d ms\n", why, urlString.c_str(), backoffMs);
        }
        return ok;
    }

    // Before an operation: false (fail fast) while backing off; once the
    // backoff is over, a new connection is tried.
    bool usable() {
        if (connected) return true;
        if (smbNetworkUp == nullptr) return false;
        if (nowMs() < retryAtMs) {
            ++fastFails;
            return false;
        }
        return reconnect("backoff over");
    }

protected:
    bool recover() override {
        if (!connected) return false;   // already down: usable() retries after the backoff
        struct smb2_stat_64 st;
        if (smb2_stat(smb2, base.c_str(), &st) == 0) return false;   // the server answers: the error was the file's
        return reconnect("no answer to a stat after an error");
    }

    uintptr_t openHandle(const char *path, int mode, int) override {
        if (!usable()) return 0;
        struct smb2fh *handle = smb2_open(smb2, full(path).c_str(), mode);
        if (handle == NULL) {
            LOG("smb2_open failed. %s\n", smb2_get_error(smb2));
            return 0;
        }
        return reinterpret_cast<uintptr_t>(handle);
    }

    void closeHandle(uintptr_t handle) override {
        if (connected) smb2_close(smb2, reinterpret_cast<struct smb2fh *>(handle));
    }

    int pwriteHandle(uintptr_t handle, const uint8_t *buf, size_t len,
                     size_t pos, bool) override {
        if (!usable()) return -EIO;
        struct smb2fh *fh = reinterpret_cast<struct smb2fh *>(handle);
        STITCH_LOG("pwrite(%d, %d)\n", (int)len, (int)pos);
        int count;
        int offset = 0;
        while (offset < len && (count = smb2_pwrite(smb2, fh, buf + offset,
                                                    len - offset, pos + offset)) != 0) {
            STITCH_LOG("smb2_pwrite(%d, %d) returned %d\n", (int)len - offset,
                (int)pos + offset, count);
            if (count == -EAGAIN) continue;
            if (count < 0) {
                LOG("Failed to write file. %s\n", smb2_get_error(smb2));
                return count;
            }
            pos += count;
            offset += count;
        }
        STITCH_LOG("pwrite(%d, %d) returning %d\n", (int)len, (int)pos, offset);
        return offset;
    }

    int preadHandle(uintptr_t handle, uint8_t *buf, size_t len, size_t pos) override {
        if (!usable()) return -EIO;
        struct smb2fh *fh = reinterpret_cast<struct smb2fh *>(handle);
        STITCH_LOG("pread(%d, %d)\n", (int)len, (int)pos);
        int count;
        int offset = 0;
        while (offset < len && (count = smb2_pread(smb2, fh, buf + offset,
                                                   len - offset, pos + offset)) != 0) {
            STITCH_LOG("smb_pread(buf, %d, %d) returned %d\n", (int)len - offset,
                (int)pos + offset, count);
            if (count == -EAGAIN) continue;
            if (count < 0) {
                LOG("Failed to read file. %s\n", smb2_get_error(smb2));
                return count;
            }
            pos += count;
            offset += count;
        }
        STITCH_LOG("pread(%d, %d) returning %d\n", (int)len, (int)pos, offset);
        return offset;
    }

    // A share operation (not through the cache): once more if the
    // connection recovers.
    template <typename Op> int retrying(Op op) {
        if (!usable()) return -EIO;
        int r = op();
        if (r < 0 && recover()) r = op();
        return r;
    }

public:
    SmbConnection(const char *u, const char *pw = NULL) : urlString(u) {
        if (pw != NULL) password = pw;
    }
    ~SmbConnection() {
        cacheCloseAll();
        if (connected) smb2_disconnect_share(smb2);
        teardown();
    }
    void connect() override {
        if (smbNetworkUp == nullptr) {
            LOG("SMB: no network in this app\n");
            return;
        }
        smbNetworkUp();
        if (!open_()) {
            backoffMs = kBackoffFirstMs;
            retryAtMs = nowMs() + backoffMs;
        }
    }
    uint32_t reconnectCount() const { return reconnects; }
    uint32_t fastFailCount() const { return fastFails; }
    int remove(const char *path) override {
        cacheCloseAll();
        return retrying([&] {
            int r = smb2_unlink(smb2, full(path).c_str());
            if (r < 0) LOG("smb2_unlink failed. %s\n", smb2_get_error(smb2));
            return r;
        });
    }
    int rename(const char *oldPath, const char *newPath) override {
        cacheCloseAll();
        return retrying([&] {
            int r = smb2_rename(smb2, full(oldPath).c_str(), full(newPath).c_str());
            if (r < 0) LOG("smb2_rename failed. %s\n", smb2_get_error(smb2));
            return r;
        });
    }
    int truncate(const char *path, size_t len) override {
        cacheCloseAll();
        return retrying([&] {
            int r = smb2_truncate(smb2, full(path).c_str(), len);
            if (r < 0) LOG("smb2_truncate failed. %s\n", smb2_get_error(smb2));
            return r;
        });
    }
    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>f) override {
        STITCH_LOG("readdir '%s'\n", path);
        if (!usable()) return 0;
        struct smb2dir *dir = smb2_opendir(smb2, full(path).c_str());
        if (dir == NULL && recover()) dir = smb2_opendir(smb2, full(path).c_str());
        if (dir == NULL) {
            LOG("readdir failed. %s\n", smb2_get_error(smb2));
            return 0;
        }
        struct smb2dirent *ent;
        int count = 0;
        while ((ent = smb2_readdir(smb2, dir))) {
            if (ent->st.smb2_type != SMB2_TYPE_FILE)
                continue;
            struct dirent de = {};
            #pragma GCC diagnostic ignored "-Wstringop-truncation"
            strncpy(de.d_name, ent->name, sizeof(de.d_name));
            de.d_type = DT_REG;
            f(count, &de, ent->st.smb2_size);
            count++;
        }
        smb2_closedir(smb2, dir);
        return count;
    }

};
