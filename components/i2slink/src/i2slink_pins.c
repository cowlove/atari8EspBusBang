/* i2slink_pin_activity(): count edges on a link's BCLK and RX pins, for
 * telling why a link doesn't lock (see i2slink.h). The PCNT pulse counter
 * counts in hardware, alongside I2S: both read the pins through the GPIO
 * matrix, so the link keeps running. */

#include "i2slink.h"

#include "driver/pulse_cnt.h"
#include "esp_rom_sys.h"

/* Rising edges on one pin over window_us; -1 if PCNT isn't available. */
static int32_t count_rising(int gpio, uint32_t window_us)
{
    pcnt_unit_config_t unit_config = {
        .low_limit = -1,
        .high_limit = 32767,
    };
    pcnt_unit_handle_t unit = NULL;
    if (pcnt_new_unit(&unit_config, &unit) != ESP_OK) {
        return -1;
    }
    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = gpio,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t chan = NULL;
    int count = -1;
    if (pcnt_new_channel(unit, &chan_config, &chan) == ESP_OK) {
        pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
        if (pcnt_unit_enable(unit) == ESP_OK) {
            pcnt_unit_clear_count(unit);
            pcnt_unit_start(unit);
            esp_rom_delay_us(window_us);
            pcnt_unit_stop(unit);
            if (pcnt_unit_get_count(unit, &count) != ESP_OK) {
                count = -1;
            }
            pcnt_unit_disable(unit);
        }
        pcnt_del_channel(chan);
    }
    pcnt_del_unit(unit);
    return count;
}

esp_err_t i2slink_pin_activity(int bclk_gpio, int rx_gpio, uint32_t window_us, i2slink_pin_activity_t *out)
{
    if (window_us == 0 || window_us > 3000) {
        window_us = 2000;   /* 16-bit counter: 10 MHz for 3 ms is 30,000 */
    }
    out->window_us = window_us;
    const int32_t bclk = count_rising(bclk_gpio, window_us);
    const int32_t rx = count_rising(rx_gpio, window_us);
    if (bclk < 0 || rx < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    out->bclk_hz = (uint32_t)((uint64_t)bclk * 1000000U / window_us);
    out->rx_edges_per_s = (uint32_t)((uint64_t)rx * 1000000U / window_us);
    return ESP_OK;
}

const char *i2slink_pin_diagnosis(const i2slink_pin_activity_t *a)
{
    const uint32_t expected = 32U * I2SLINK_FRAMES_PER_SECOND;
    if (a->bclk_hz == 0) {
        return "no BCLK: the clock source isn't running its link, or the BCLK wire is open";
    }
    if (a->bclk_hz < expected / 2 || a->bclk_hz > expected * 2) {
        return "BCLK at the wrong rate: the two ends' I2SLINK_FRAMES_PER_SECOND differ?";
    }
    if (a->rx_edges_per_s == 0) {
        return "BCLK ok, RX silent: the peer isn't transmitting, or the RX wire is open";
    }
    return "BCLK and RX both active but no lock: frame rate or RX edge mismatch between "
           "the builds, or a noisy RX wire";
}
