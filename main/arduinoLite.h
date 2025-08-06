#pragma once
#include "hal/gpio_hal.h"
#include "soc/soc_caps.h"
#include <cstring>
#include "hal/ledc_types.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include <algorithm>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#ifndef ARDUINO
static inline void delay(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static inline void yield() { delay(0); }
static inline void digitalWrite(int pin, int val) {
   gpio_set_level((gpio_num_t)pin, val);
}
static inline int digitalRead(int pin) { return gpio_get_level((gpio_num_t)pin); }
static inline void disableCore0WDT() {}
static inline void enableCore0WDT() {}
static inline unsigned long millis() { 
     return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

struct ArduinoSerial {
   inline bool available() { 
      return false;
      // TODO: this doesn't work 
      size_t len = 0;
      uart_get_buffered_data_len(UART_NUM_0, &len);
      return len > 0;
   }
   inline int read() { 
      uint8_t data;
      int len = uart_read_bytes(UART_NUM_0, &data, 1, 0);
      if (len > 0) return data;
      return -1;
   }
};
extern ArduinoSerial Serial;


#define INPUT 0x01
#define OUTPUT            0x03
#define PULLUP            0x04
#define INPUT_PULLUP      0x05
#define PULLDOWN          0x08
#define INPUT_PULLDOWN    0x09
#define OPEN_DRAIN        0x10
#define OUTPUT_OPEN_DRAIN 0x13
#define ANALOG            0xC0

#define log_w printf
#define log_e printf
#define log_i printf

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wattributes"

static inline void pinMode(uint8_t p, uint8_t m) {
   gpio_num_t pin = (gpio_num_t)p;
   gpio_mode_t mode = (gpio_mode_t)m;
   gpio_hal_context_t gpiohal;
   gpiohal.dev = GPIO_LL_GET_HW(GPIO_PORT_0);

   gpio_config_t conf = {
      .pin_bit_mask = (1ULL << pin),              /*!< GPIO pin: set with bit mask, each bit maps to a GPIO */
      .mode = GPIO_MODE_DISABLE,                  /*!< GPIO mode: set input/output mode                     */
      .pull_up_en = GPIO_PULLUP_DISABLE,          /*!< GPIO pull-up                                         */
      .pull_down_en = GPIO_PULLDOWN_DISABLE,      /*!< GPIO pull-down                                       */
      .intr_type = (gpio_int_type_t)gpiohal.dev->pin[pin].int_type /*!< GPIO interrupt type - previously set                 */
   };
   if (mode < 0x20) {  //io
      conf.mode = (gpio_mode_t)(mode & (INPUT | OUTPUT));
      if (mode & OPEN_DRAIN) {
         conf.mode = (gpio_mode_t)(conf.mode | GPIO_MODE_DEF_OD);
      }
      if (mode & PULLUP) {
         conf.pull_up_en = GPIO_PULLUP_ENABLE;
      }
      if (mode & PULLDOWN) {
         conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
      }
   }
   if (gpio_config(&conf) != ESP_OK) {
      log_e("IO %i config failed", pin);
      return;
   }
}

static inline bool ledcAttachChannel(uint8_t pin, uint32_t freq, uint8_t resolution, uint8_t channel) {
   ledc_timer_config_t ledc_timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = (ledc_timer_bit_t)resolution,
      .timer_num = (ledc_timer_t)channel,
      .freq_hz = freq,
      .clk_cfg = LEDC_AUTO_CLK,
   };
   ledc_timer_config(&ledc_timer);

   ledc_channel_config_t ledc_channel = {0};
   ledc_channel.channel = (ledc_channel_t)channel;
   ledc_channel.duty = 0;
   ledc_channel.gpio_num = pin;
   ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
   ledc_channel.timer_sel = (ledc_timer_t)channel;
   ledc_channel_config(&ledc_channel);
   return true;
}

static inline void ledcWrite(int channel, int duty) {
   ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, duty);
   ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel);
}


using std::min;
using std::max;


#endif
