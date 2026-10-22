#pragma once
#include "diskStitch.h"

#define SMB
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#ifndef CSIM
#include "util.h"
#endif

class SmbConnection : public StorageInterface {
    struct smb2_context *smb2 = NULL;
    struct smb2_url *url = NULL;
    struct smb2fh *fh = NULL;
    string lastFile;
    string password, urlString;
    public: 
    SmbConnection(const char *u, const char *pw = NULL) : urlString(u) {
        if (pw != NULL) password = pw;
    }
    ~SmbConnection() {
        //smb2_destroy_url(url);
        //smb2_destroy_context(smb2);
    }
    void connect() {
        wifiRun();
        smb2 = smb2_init_context();
        if (password != "")
            smb2_set_password(smb2, password.c_str());
        url = smb2_parse_url(smb2, urlString.c_str());
        smb2_set_timeout(smb2, 3/*seconds*/);
        if (smb2_connect_share(smb2, url->server, url->share, url->user) < 0) {
            printf("smb2_connect_share failed. %s\n", smb2_get_error(smb2));
        }
    }
    int remove(const char *path) { 
        cacheCloseAll();
        int r = smb2_unlink(smb2, path);
        if (r < 0) 
            printf("smb2_unlink failed. %s\n", smb2_get_error(smb2));
        return r;
    }
    int truncate(const char *path, size_t len) { 
        int r = smb2_truncate(smb2, path, len);
        if (r < 0) 
            printf("smb2_truncate failed. %s\n", smb2_get_error(smb2));
        return r;
    }
    int readdir(const char *path, std::function<void(int, struct dirent *, size_t)>f) {
        LOG("readdir '%s'\n", path);
        struct smb2dir *dir;
        struct smb2dirent *ent;
        dir = smb2_opendir(smb2, path);
        if (dir == NULL) { 
            printf("readdir failed. %s\n", smb2_get_error(smb2));
            return 0;
        }
        int count = 0;
        while ((ent = smb2_readdir(smb2, dir))) {
            if (ent->st.smb2_type != SMB2_TYPE_FILE)
                continue;
            struct dirent de = {0};
            strncpy(de.d_name, ent->name, sizeof(de.d_name));
            de.d_type = DT_REG;
            f(count, &de, ent->st.smb2_size);
            count++;
        }
        smb2_closedir(smb2, dir);
        return count;
    }

    static const int cacheSize = 4;
    struct CacheEntry {
        struct smb2fh *fh = NULL;
        string name;
        int mode = 0;
        void clear() { name = ""; mode = 0; fh = NULL; }    
    } fhCache[cacheSize];

    int cacheMisses = 0;
    struct smb2fh *cacheOpen(const char *path, int mode) {
        if (fhCache[0].name == path && fhCache[0].mode == mode)
            return fhCache[0].fh;
        for(int i = 1; i < cacheSize; i++) {
            if(fhCache[i].name == path && fhCache[i].mode == mode) {
                CacheEntry ce = fhCache[i];
                for(int j = i; j > 0; j--) 
                    fhCache[j] = fhCache[j - 1]; 
                fhCache[0] = ce;
                return fhCache[0].fh;
            }
        }
        struct smb2fh *f = smb2_open(smb2, path, mode);
        if (f == NULL) {
            printf("smb2_open failed. %s\n", smb2_get_error(smb2));
            return NULL;
        }
        cacheMisses++;
        //printf("cacheOpen misses %d\n", cacheMisses);
        if(fhCache[cacheSize - 1].fh != NULL)
            smb2_close(smb2, fhCache[cacheSize - 1].fh);
        for(int j = cacheSize - 1; j > 0; j--) 
            fhCache[j] = fhCache[j - 1]; 
        fhCache[0].fh = f;
        fhCache[0].name = path;
        fhCache[0].mode = mode;
        return f;
    }

    void cacheCloseAll() { 
        for(int i = 0; i < cacheSize; i++) {
            if(fhCache[i].fh != NULL)
                smb2_close(smb2, fhCache[i].fh);
            fhCache[i].clear();
        }
    }

    int open(const char *path, int mode, int perm = 0) { 
        LOG("open('%s')\n", path);
        fh = cacheOpen(path, mode);
        return fh == NULL ? -1 : 0;
    }
    int pwrite(const uint8_t *buf, size_t len, size_t pos, bool eof) { 
        LOG("pwrite(%d, %d)\n", (int)len, (int)pos);
        int count;
        int offset = 0;
        while (offset < len && (count = smb2_pwrite(smb2, fh, buf + offset, len - offset, pos + offset)) != 0) {
            LOG("smb2_pwrite(%d, %d) returned %d\n", (int)len - offset, (int)pos + offset, count);
            if (count == -EAGAIN) {
                continue;
            }
            if (count < 0) {
                printf("Failed to write file. %s\n", smb2_get_error(smb2));
                return count;
            }
            pos += count;
            offset += count;
        }
        LOG("pwrite(%d, %d) returning %d\n", (int)len, (int)pos, offset);
        return offset;
    }
    int pread(uint8_t *buf, size_t len, size_t pos) { 
        LOG("pread(%d, %d)\n", (int)len, (int)pos);
        int count;
        int offset = 0;
        while (offset < len && (count = smb2_pread(smb2, fh, buf + offset, len - offset, pos + offset)) != 0) {
            LOG("smb_pread(buf, %d, %d) returned %d\n", (int)len - offset, (int)pos + offset, count);
            if (count == -EAGAIN) {
                continue;
            }
            if (count < 0) {
                printf("Failed to read file. %s\n", smb2_get_error(smb2));
                return count;
            }
            //for(int n = 0; n < count; n++) putchar(buf[n]);
            //write(0, buf, count);
            pos += count;
            offset += count;
        }
        LOG("pread(%d, %d) returning %d\n", (int)len, (int)pos, offset);
        return offset;
    }
    void close() { 
        //if (fh != NULL) smb2_close(smb2, fh);
        //fh = NULL;
    }
};
