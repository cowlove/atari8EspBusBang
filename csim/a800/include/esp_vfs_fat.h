#pragma once
// csim: nop FAT-on-SD mount stubs: no card (ESP_ERR_TIMEOUT), so /sd is
// never mounted.
#include "driver/sdspi_host.h"

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_sdmmc_mount_config_t;

static inline esp_err_t esp_vfs_fat_sdmmc_mount(const char *, const sdmmc_host_t *, const void *,
                                                const esp_vfs_fat_sdmmc_mount_config_t *, sdmmc_card_t **out)
{ *out = nullptr; return ESP_ERR_TIMEOUT; }
static inline esp_err_t esp_vfs_fat_sdspi_mount(const char *, const sdmmc_host_t *, const sdspi_device_config_t *,
                                                const esp_vfs_fat_sdmmc_mount_config_t *, sdmmc_card_t **out)
{ *out = nullptr; return ESP_ERR_TIMEOUT; }
static inline esp_err_t esp_vfs_fat_sdcard_unmount(const char *, sdmmc_card_t *) { return ESP_OK; }
