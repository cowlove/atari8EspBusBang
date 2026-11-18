#pragma once
#include "csim.h"

// GPIO numbers and the pin reset, for code that names pins (the Sidecar's
// SD card): csim has no pads, so resetting one does nothing.
#ifndef GPIO_NUM_0
enum : int {
    GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
    GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14,
    GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_20, GPIO_NUM_21,
    GPIO_NUM_33 = 33, GPIO_NUM_34, GPIO_NUM_35, GPIO_NUM_36, GPIO_NUM_37, GPIO_NUM_38, GPIO_NUM_39,
    GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_43, GPIO_NUM_44, GPIO_NUM_45, GPIO_NUM_46,
    GPIO_NUM_47, GPIO_NUM_48,
};
#define GPIO_NUM_0 GPIO_NUM_0
#endif
static inline esp_err_t gpio_reset_pin(gpio_num_t) { return ESP_OK; }
