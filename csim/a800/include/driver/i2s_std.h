#pragma once
// CSIM: the slice of ESP-IDF's I2S standard-mode driver API that
// components/sidecar_link/i2slink.c uses. The channels are emulated in
// csim/a800/csim_i2s.cpp (clocking, DMA rings and descriptor write-back);
// configuration fields the emulation doesn't need are accepted and ignored.
#include "csim.h"

typedef struct csim_i2s_chan *i2s_chan_handle_t;

typedef enum { I2S_NUM_0 = 0, I2S_NUM_1 = 1 } i2s_port_t;
typedef enum { I2S_ROLE_MASTER, I2S_ROLE_SLAVE } i2s_role_t;
typedef enum { I2S_DATA_BIT_WIDTH_16BIT = 16 } i2s_data_bit_width_t;
typedef enum { I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2 } i2s_slot_mode_t;
typedef enum { I2S_CLK_SRC_DEFAULT = 0, I2S_CLK_SRC_PLL_240M = 1 } i2s_clock_src_t;
typedef int i2s_mclk_multiple_t;
#define I2S_GPIO_UNUSED (-1)

typedef struct {
    int id;
    i2s_role_t role;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
    bool auto_clear;
} i2s_chan_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(port, chan_role) \
    { .id = (int)(port), .role = (chan_role), .dma_desc_num = 6, .dma_frame_num = 240, .auto_clear = false }

typedef struct {
    uint32_t sample_rate_hz;
    i2s_clock_src_t clk_src;
    i2s_mclk_multiple_t mclk_multiple;
} i2s_std_clk_config_t;
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) \
    { .sample_rate_hz = (rate), .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = 256 }

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_mode_t slot_mode;
} i2s_std_slot_config_t;
#define I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits, mode) { .data_bit_width = (bits), .slot_mode = (mode) }

typedef struct {
    uint32_t mclk_inv : 1;
    uint32_t bclk_inv : 1;
    uint32_t ws_inv : 1;
} i2s_std_gpio_invert_flags_t;

typedef struct {
    int mclk, bclk, ws, dout, din;
    i2s_std_gpio_invert_flags_t invert_flags;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *config);
esp_err_t i2s_channel_disable(i2s_chan_handle_t channel);
esp_err_t i2s_del_channel(i2s_chan_handle_t channel);
