// See atariReset.h.
#include "atariReset.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace atariReset {
namespace {
constexpr gpio_num_t kResetGpio = GPIO_NUM_3;
bool isHeld = false;
}  // namespace

void begin()
{
    gpio_reset_pin(kResetGpio);
    gpio_set_pull_mode(kResetGpio, GPIO_FLOATING);   // the Atari pulls RESET up (to 5 V, through R1)
    hold();   // main isn't serving the bus yet
}

void hold()
{
    gpio_set_level(kResetGpio, 0);
    gpio_set_direction(kResetGpio, GPIO_MODE_INPUT_OUTPUT_OD);
    isHeld = true;
}

void release()
{
    // An input again, so it never drives the line, high or low.
    gpio_set_direction(kResetGpio, GPIO_MODE_INPUT);
    isHeld = false;
}

void pulse(int ms)
{
    hold();
    vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 1));
    release();
}

bool held()
{
    return isHeld;
}
}  // namespace atariReset
