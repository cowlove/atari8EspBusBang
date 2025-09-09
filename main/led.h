#include "driver/rmt_tx.h"

struct LedRmt {
    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = (gpio_num_t)ledPin,
        .clk_src = RMT_CLK_SRC_DEFAULT, // select source clock
        .resolution_hz = 10000000,
        .mem_block_symbols = 64, // increase the block size can make the LED less flickering
        .trans_queue_depth = 4, // set the number of transactions that can be pending in the background
    };
    rmt_channel_handle_t led_chan = NULL;
    rmt_encoder_handle_t encoder = NULL;
    const rmt_copy_encoder_config_t encoder_cfg = {};
    
    void init() { 
        pinMode(ledPin, OUTPUT);
        digitalWrite(ledPin, 0);
        ESP_LOGI(TAG, "Create RMT TX channel");
        ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));
        ESP_LOGI(TAG, "Create simple callback-based encoder");
        ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder_cfg, &encoder));
        ESP_LOGI(TAG, "Enable RMT TX channel");
        ESP_ERROR_CHECK(rmt_enable(led_chan));
    }
    rmt_symbol_word_t led_data[3 * 8] = {0};

    void write(uint8_t red_val, uint8_t green_val, uint8_t blue_val) {
        rmt_transmit_config_t tx_config = {
            .loop_count = 0, // no transfer loop
        };

        // default WS2812B color order is G, R, B
        int color[3] = {green_val, red_val, blue_val};
        int i = 0;
        for (int col = 0; col < 3; col++) {
            for (int bit = 0; bit < 8; bit++) {
                if ((color[col] & (1 << (7 - bit)))) {
                    // HIGH bit
                    led_data[i].level0 = 1;     // T1H
                    led_data[i].duration0 = 8;  // 0.8us
                    led_data[i].level1 = 0;     // T1L
                    led_data[i].duration1 = 4;  // 0.4us
                } else {
                    // LOW bit
                    led_data[i].level0 = 1;     // T0H
                    led_data[i].duration0 = 4;  // 0.4us
                    led_data[i].level1 = 0;     // T0L
                    led_data[i].duration1 = 8;  // 0.8us
                }
                i++;
            }
        }
        ESP_ERROR_CHECK(rmt_transmit(led_chan, encoder, led_data, sizeof(led_data), &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
    }
};
struct LedDedic {
    void init() { 
        pinMode(ledPin, OUTPUT);
        digitalWrite(ledPin, 0);
        if(1) { 
            static dedic_gpio_bundle_handle_t bundleIn, bundleOut;
            int bundleB_gpios[] = {ledPin};
            dedic_gpio_bundle_config_t bundleB_config = {
                .gpio_array = bundleB_gpios,
                .array_size = sizeof(bundleB_gpios) / sizeof(bundleB_gpios[0]),
                .flags = {
                    .out_en = 1
                },
            };
            ESP_ERROR_CHECK(dedic_gpio_new_bundle(&bundleB_config, &bundleOut));
        }
        //gpio_matrix_out(ledPin, CORE1_GPIO_OUT0_IDX, false, false);
    }
    void write(uint8_t red_val, uint8_t green_val, uint8_t blue_val) {
        //busyWaitCCount(100);
        //return;       
        uint32_t stsc;
        int color[3] = {green_val, red_val, blue_val};
        int longCycles = 175;
        int shortCycles = 90;
        int i = 0;
        for (int col = 0; col < 3; col++) {
            for (int bit = 0; bit < 8; bit++) {
                if ((color[col] & (1 << (7 - bit)))) {
                    dedic_gpio_cpu_ll_write_all(1);
                    stsc = XTHAL_GET_CCOUNT();
                    while(XTHAL_GET_CCOUNT() - stsc < longCycles) {}

                    dedic_gpio_cpu_ll_write_all(0);
                    stsc = XTHAL_GET_CCOUNT();
                    while(XTHAL_GET_CCOUNT() - stsc < shortCycles) {}
                } else {
                    // LOW bit
                    dedic_gpio_cpu_ll_write_all(1);
                    stsc = XTHAL_GET_CCOUNT();
                    while(XTHAL_GET_CCOUNT() - stsc < shortCycles) {}

                    dedic_gpio_cpu_ll_write_all(0);
                    stsc = XTHAL_GET_CCOUNT();
                    while(XTHAL_GET_CCOUNT() - stsc < longCycles) {}
                }
                i++;
            }
        }
    }
};
