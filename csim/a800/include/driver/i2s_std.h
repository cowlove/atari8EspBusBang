#ifndef A8BB_CSIM_DRIVER_I2S_STD_H
#define A8BB_CSIM_DRIVER_I2S_STD_H

#include "csim.h"

typedef void *i2s_chan_handle_t;
typedef int i2s_port_t;
typedef int i2s_role_t;
typedef int i2s_data_bit_width_t;
typedef int i2s_slot_mode_t;
typedef int i2s_mclk_multiple_t;

typedef struct {
    int dma_desc_num;
    int dma_frame_num;
} i2s_chan_config_t;

typedef struct {
    int mclk_multiple;
} i2s_std_clk_config_t;

typedef struct {
    int unused;
} i2s_std_slot_config_t;

typedef struct {
    int mclk;
    int bclk;
    int ws;
    int dout;
    int din;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_MCLK_MULTIPLE_128 128
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
#define I2S_GPIO_UNUSED (-1)
#define GPIO_NUM_48 48
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) ((i2s_chan_config_t){0, 0})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) ((i2s_std_clk_config_t){0})
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, mode) ((i2s_std_slot_config_t){0})

static inline esp_err_t i2s_new_channel(const i2s_chan_config_t *config,
                                        i2s_chan_handle_t *tx_channel,
                                        i2s_chan_handle_t *rx_channel)
{
    (void)config;
    (void)rx_channel;
    if (tx_channel) {
        *tx_channel = NULL;
    }
    return ESP_OK;
}

static inline esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel,
                                                   const i2s_std_config_t *config)
{
    (void)channel;
    (void)config;
    return ESP_OK;
}

static inline esp_err_t i2s_channel_enable(i2s_chan_handle_t channel)
{
    (void)channel;
    return ESP_OK;
}

#endif
