#pragma once
#include <fcntl.h>

typedef int spiffs_file;

#define SPIFFS_O_TRUNC O_TRUNC
#define SPIFFS_O_RDONLY O_RDONLY
#define SPIFFS_O_RDWR O_RDWR
#define SPIFFS_O_CREAT O_CREAT

static inline int SPIFFS_write(spiffs_t *, int, void *, int) { return 0; }
static inline int SPIFFS_read(spiffs_t *, int, void *, int) { return 0; }
static inline int SPIFFS_open(spiffs_t *, const char *, int, int) { return 0; }
static inline int SPIFFS_close(spiffs_t *, int) { return 0; }

