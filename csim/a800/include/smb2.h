#pragma once
#include "csim.h"
struct smb2_context {};
struct smb2_url { const char *server; const char *share; const char *user; };
struct smb2fh {};
struct smb2dir {};
struct smb2dirent { struct { int smb2_type; uint64_t smb2_size; } st; const char *name; };
#define SMB2_TYPE_FILE 1
#define SMB2_NEGOTIATE_SIGNING_ENABLED 1
static inline smb2_context *smb2_init_context(void) { return nullptr; }
static inline smb2_url *smb2_parse_url(smb2_context *, const char *) { return nullptr; }
static inline const char *smb2_get_error(smb2_context *) { return "CSIM"; }
static inline int smb2_set_password(smb2_context *, const char *) { return 0; }
static inline int smb2_set_security_mode(smb2_context *, int) { return 0; }
static inline int smb2_set_timeout(smb2_context *, int) { return 0; }
static inline int smb2_connect_share(smb2_context *, const char *, const char *, const char *) { return 0; }
static inline int smb2_unlink(smb2_context *, const char *) { return -1; }
static inline int smb2_rename(smb2_context *, const char *, const char *) { return -1; }
static inline int smb2_truncate(smb2_context *, const char *, uint64_t) { return -1; }
static inline smb2dir *smb2_opendir(smb2_context *, const char *) { return nullptr; }
static inline smb2dirent *smb2_readdir(smb2_context *, smb2dir *) { return nullptr; }
static inline void smb2_closedir(smb2_context *, smb2dir *) {}
static inline smb2fh *smb2_open(smb2_context *, const char *, int) { return nullptr; }
static inline int smb2_close(smb2_context *, smb2fh *) { return 0; }
static inline int smb2_pwrite(smb2_context *, smb2fh *, const void *, size_t, uint64_t) { return -1; }
static inline int smb2_pread(smb2_context *, smb2fh *, void *, size_t, uint64_t) { return -1; }
static inline void smb2_destroy_context(smb2_context *) {}
