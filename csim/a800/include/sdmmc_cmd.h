#pragma once
// csim: nop SD card commands: no card (ESP_ERR_TIMEOUT, as an empty slot).
#include "driver/sdmmc_host.h"

static inline esp_err_t sdmmc_card_init(const sdmmc_host_t *, sdmmc_card_t *) { return ESP_ERR_TIMEOUT; }
static inline esp_err_t sdmmc_read_sectors(sdmmc_card_t *, void *, size_t, size_t) { return ESP_ERR_TIMEOUT; }
static inline esp_err_t sdmmc_write_sectors(sdmmc_card_t *, const void *, size_t, size_t) { return ESP_ERR_TIMEOUT; }
