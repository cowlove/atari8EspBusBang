#pragma once
// csim: nop SPI bus stubs (the Sidecar's SD card in SPI mode). The bus
// "initializes"; nothing is ever attached to it.
#include "csim.h"

typedef int spi_host_device_t;
#define SPI2_HOST 1
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;
static inline esp_err_t spi_bus_initialize(spi_host_device_t, const spi_bus_config_t *, int) { return ESP_OK; }
static inline esp_err_t spi_bus_free(spi_host_device_t) { return ESP_OK; }
