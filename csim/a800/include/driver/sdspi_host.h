#pragma once
// csim: nop SD-over-SPI stubs (see driver/sdmmc_host.h: no card).
#include "driver/sdmmc_host.h"
#include "driver/spi_common.h"

typedef int sdspi_dev_handle_t;
#define SDSPI_DEFAULT_DMA 3
#define SDSPI_HOST_DEFAULT() (sdmmc_host_t{SPI2_HOST, SDMMC_FREQ_DEFAULT})
typedef struct {
    spi_host_device_t host_id;
    gpio_num_t gpio_cs;
} sdspi_device_config_t;
#define SDSPI_DEVICE_CONFIG_DEFAULT() (sdspi_device_config_t{})

static inline esp_err_t sdspi_host_init(void) { return ESP_OK; }
static inline esp_err_t sdspi_host_init_device(const sdspi_device_config_t *, sdspi_dev_handle_t *out) { *out = 0; return ESP_OK; }
static inline esp_err_t sdspi_host_remove_device(sdspi_dev_handle_t) { return ESP_OK; }
static inline esp_err_t sdspi_host_deinit(void) { return ESP_OK; }
