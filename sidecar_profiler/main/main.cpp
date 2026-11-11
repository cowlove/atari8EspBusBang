// Sidecar profiler: samples the Atari bus pins the Sidecar board is wired to
// and prints timing histograms every few seconds, while the host and the
// Atari run normally (components/bus_profiler). Firmware for the Sidecar
// board in place of the Sidecar app; the host must not run the Sidecar link
// (its sdkconfig has A8BB_SIDECAR_ENABLE off), so the link wires are free.
//
// Wiring, Sidecar pin <- host board pin (3.3 V side; inputs only, no pulls):
//
//   Sidecar  host    signal        bundle bit
//   GPIO4    GPIO36  phi2 clock    0
//   GPIO36   GPIO48  profile pin   1   (the rigs' BCLK wire, already there)
//   GPIO5    GPIO17  R/W           2
//   GPIO6    GPIO47  extSel_       3   (open drain on the host: keep it short)
//   GPIO7    GPIO38  D0            4
//   GPIO15   GPIO39  D1            5
//   GPIO16   GPIO1   A0            6
//   GPIO35   GPIO21  refresh_      7   (the rigs' Sidecar-TX wire, already
//                                       there: never an output here)
//
// The new pins avoid the strapping pins (0, 3, 45, 46), USB (19/20), the
// console UART (43/44), flash and PSRAM (26-32), the RGB LED (38/48) and
// the planned link pins (9, 10, 46). GPIO37 (from the host's TX, GPIO0)
// stays unconfigured.
//
// The profile pin's channel is only meaningful with a host build that
// drives it (PROFILE_MODE=PIN or a profile point); otherwise the host's
// status LED data or nothing shows there.
//
// SIDECAR_PROFILER_SELFTEST=1 (idf.py -DSIDECAR_PROFILER_SELFTEST=1 build):
// no wiring needed; the clock pin also outputs a ~1.78 MHz square wave
// (LEDC), so the capture should find clock edges near 66 and 132 ticks.

#include <stdint.h>
#include <stdio.h>

#include "bus_profiler.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef SIDECAR_PROFILER_SELFTEST
#define SIDECAR_PROFILER_SELFTEST 0
#endif

namespace {

constexpr BusProfilerPins pins = {
    .clock = 4, .profile = 36, .rw = 5, .extSel = 6,
    .d0 = 7, .d1 = 15, .a0 = 16, .refresh = 35,
};

constexpr uint32_t cpuHz = 240 * 1000000;
constexpr uint32_t atariHz = 1780000;           // NTSC phi2, ~1.79 MHz
constexpr uint32_t halfCycleTicks = cpuHz / atariHz / 2;
constexpr uint32_t chunkCycles = cpuHz / 20;    // 50 ms per masked chunk
constexpr uint32_t reportSeconds = 10;

DRAM_ATTR Hist2 hist[busProfilerChannels];
DRAM_ATTR BusProfilerState state = {};
uint32_t inputOffset = 0;

// One masked chunk of captures. Skip phases always: a capture's fixed
// length would otherwise lock onto any periodic 6502 loop.
IRAM_ATTR void captureChunk() {
    busProfilerCaptureForCycles<true, true, true, halfCycleTicks>(
        state, hist, inputOffset, chunkCycles);
}

// Clock edges seen in the bundle in ~100 us (~178 at 1.78 MHz). The
// capture waits for a fall with interrupts masked, so it only runs while
// the clock is toggling: an unplugged or stopped Atari would hang it.
uint32_t clockEdges() {
    const uint32_t clockMask = 1u << inputOffset;
    const uint32_t start = XTHAL_GET_CCOUNT();
    uint32_t edges = 0, last = dedic_gpio_cpu_ll_read_in() & clockMask;
    while (XTHAL_GET_CCOUNT() - start < cpuHz / 10000) {
        const uint32_t now = dedic_gpio_cpu_ll_read_in() & clockMask;
        if (now != last) ++edges;
        last = now;
    }
    return edges;
}

void configureInputs() {
    const int list[] = {pins.clock, pins.profile, pins.rw, pins.extSel,
                        pins.d0, pins.d1, pins.a0, pins.refresh};
    uint64_t mask = 0;
    for (int pin : list) mask |= 1ULL << pin;
    const gpio_config_t config = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

#if SIDECAR_PROFILER_SELFTEST
void startSelfTestClock() {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_1_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = atariHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel = {};
    channel.gpio_num = pins.clock;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 1;                               // 50% at 1-bit resolution
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    // LEDC made the pin an output only; read it back too. Not
    // gpio_set_direction(INPUT_OUTPUT): enabling the output again routes
    // the pin back to the plain GPIO output, cutting LEDC off.
    ESP_ERROR_CHECK(gpio_input_enable(static_cast<gpio_num_t>(pins.clock)));
    printf("self-test: %" PRIu32 " Hz square wave on GPIO%d\n", atariHz, pins.clock);
}
#endif

void profilerTask(void *) {
    // The bundle belongs to the core that creates it: this one.
    inputOffset = busProfilerInitBundle(pins);
#if SIDECAR_PROFILER_SELFTEST
    startSelfTestClock();
#endif
    printf("clock: %" PRIu32 " edges in 100 us\n", clockEdges());
    uint64_t captures = 0, noClockFall = 0;
    uint32_t seconds = 0, chunks = 0;
    for (;;) {
        if (clockEdges() < 20) {
            printf("no clock on GPIO%d: waiting\n", pins.clock);
            while (clockEdges() < 20) vTaskDelay(pdMS_TO_TICKS(1000));
            printf("clock: %" PRIu32 " edges in 100 us\n", clockEdges());
        }
        portDISABLE_INTERRUPTS();
        captureChunk();
        portENABLE_INTERRUPTS();
        captures += state.result.captures;
        noClockFall += state.result.noClockFall;
        if (++chunks < reportSeconds * 20) continue;
        chunks = 0;
        seconds += reportSeconds;
        printf("DUMP %" PRIu32 " s: %" PRIu64 " captures, %" PRIu64 " without clock fall\n",
               seconds, captures, noClockFall);
        busProfilerPrintHistograms(hist, busProfilerChannels);
        busProfilerPrintCaptures(state, pins, false);
    }
}

}  // namespace

extern "C" void app_main(void)
{
    printf("sidecar_profiler: bus pins clock %d, profile %d, R/W %d, extSel %d, "
           "D0 %d, D1 %d, A0 %d, refresh %d; report every %" PRIu32 " s\n",
           pins.clock, pins.profile, pins.rw, pins.extSel, pins.d0, pins.d1,
           pins.a0, pins.refresh, reportSeconds);
    configureInputs();
    // Core 1, never blocking between chunks.
    xTaskCreatePinnedToCore(profilerTask, "profiler", 4096, nullptr, tskIDLE_PRIORITY + 10,
                            nullptr, 1);
}
