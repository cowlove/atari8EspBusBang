#pragma once

#include "csim.h"

int SPIFFS_stat(spiffs_t *fs, const char *path, spiffs_stat *stat);
spiffs_file SPIFFS_open(spiffs_t *fs, const char *path, int flags, int mode);
int SPIFFS_close(spiffs_t *fs, spiffs_file fd);
int SPIFFS_read(spiffs_t *fs, spiffs_file fd, void *buf, int size);
int SPIFFS_write(spiffs_t *fs, spiffs_file fd, const void *buf, int size);
int SPIFFS_lseek(spiffs_t *fs, spiffs_file fd, int offset, int whence);
spiffs_DIR *SPIFFS_opendir(spiffs_t *fs, const char *path, spiffs_DIR *dir);
spiffs_dirent *SPIFFS_readdir(spiffs_DIR *dir, spiffs_dirent *entry);
void SPIFFS_closedir(spiffs_DIR *dir);
