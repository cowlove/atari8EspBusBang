#pragma once
// csim: nop SDMMC stubs. There is no card: every card-level call answers
// as an empty slot does on hardware (ESP_ERR_TIMEOUT), so the Sidecar runs
// without /sd, as it does with no card fitted.
#include "csim.h"

#define SDMMC_FREQ_DEFAULT 20000
#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP (1 << 0)

typedef struct {
    int slot;
    int max_freq_khz;
} sdmmc_host_t;
#define SDMMC_HOST_DEFAULT() (sdmmc_host_t{1, SDMMC_FREQ_DEFAULT})

typedef struct {
    gpio_num_t clk, cmd, d0, d1, d2, d3;
    int width;
    unsigned flags;
} sdmmc_slot_config_t;
#define SDMMC_SLOT_CONFIG_DEFAULT() (sdmmc_slot_config_t{})

typedef struct {
    struct { uint32_t capacity; uint32_t sector_size; } csd;
    struct { char name[8]; } cid;
    int real_freq_khz;
} sdmmc_card_t;

static inline esp_err_t sdmmc_host_init(void) { return ESP_OK; }
static inline esp_err_t sdmmc_host_init_slot(int, const sdmmc_slot_config_t *) { return ESP_OK; }
static inline esp_err_t sdmmc_host_deinit(void) { return ESP_OK; }
