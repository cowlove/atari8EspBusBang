// See mainReset.h.
#include "mainReset.h"

#include "atariReset.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace mainReset {
namespace {
constexpr gpio_num_t kEnGpio = GPIO_NUM_11;
constexpr gpio_num_t kBootGpio = GPIO_NUM_9;

void openDrain(gpio_num_t pin, int level)
{
    gpio_set_level(pin, level);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
}
}  // namespace

void reset()
{
    // The Atari waits in reset until main is serving the bus again (its
    // BOOT COMPLETE: atariReset.h).
    atariReset::hold();
    // The link stays up: its RX listens on GPIO9 through the GPIO matrix;
    // IO0 released (open drain, high) beside that, then an input again.
    openDrain(kBootGpio, 1);
    openDrain(kEnGpio, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    openDrain(kEnGpio, 1);   // released: the main board's RC brings EN up
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_direction(kBootGpio, GPIO_MODE_INPUT);
}
}  // namespace mainReset
