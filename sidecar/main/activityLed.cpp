#include "activityLed.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#include <cstdint>

namespace {

const char *TAG = "led";

rmt_channel_handle_t channel = nullptr;
rmt_encoder_handle_t encoder = nullptr;

// Activity since the last frame, one bit per LedActivity, and the fading
// flash: its color and level (0..255).
uint32_t pending = 0;
uint8_t flashR = 0, flashG = 0, flashB = 0;
int flashLevel = 0;

int64_t nextFrame = 0;
int64_t lastStallUs = 0;
uint32_t lastStalls = 0;
uint8_t frame[3];   // G, R, B (the SK6812's order)
uint8_t shown[3] = {1, 1, 1};   // anything but the first frame

constexpr uint32_t kResolutionHz = 10'000'000;   // 0.1 us ticks
constexpr int64_t kFrameUs = 20'000;
constexpr int kFlashLevel = 255;
constexpr int kDecayPerFrame = 40;   // a flash fades in ~130 ms

}  // namespace

void activityLedBegin()
{
    rmt_tx_channel_config_t cfg = {};
    cfg.gpio_num = static_cast<gpio_num_t>(SIDECAR_LED_GPIO);
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = kResolutionHz;
    cfg.mem_block_symbols = 48;      // one frame: 24 bits
    cfg.trans_queue_depth = 2;
    esp_err_t err = rmt_new_tx_channel(&cfg, &channel);
    // WS2812/SK6812 bit timings, in 0.1 us ticks: 0 = 0.3 high, 0.9 low;
    // 1 = 0.9 high, 0.3 low (both chips accept these). MSB first.
    rmt_bytes_encoder_config_t bytes = {};
    bytes.bit0 = {.duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0};
    bytes.bit1 = {.duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0};
    bytes.flags.msb_first = 1;
    if (err == ESP_OK) err = rmt_new_bytes_encoder(&bytes, &encoder);
    if (err == ESP_OK) err = rmt_enable(channel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "activity LED on GPIO%d unavailable: %s", SIDECAR_LED_GPIO, esp_err_to_name(err));
        channel = nullptr;
        return;
    }
    ESP_LOGI(TAG, "activity LED on GPIO%d", SIDECAR_LED_GPIO);
}

void activityLed(LedActivity what)
{
    pending |= 1u << static_cast<unsigned>(what);
}

void activityLedPoll(int64_t nowUs, const i2slink_t *link)
{
    if (channel == nullptr || nowUs < nextFrame) return;
    nextFrame = nowUs + kFrameUs;
    i2slink_stats_t stats;
    i2slink_stats(link, &stats);
    // Up: locked onto main's frames, and the frame clock hasn't stopped
    // (a stalled RX DMA in the last second: main is off or resetting).
    const uint32_t stalls = stats.reads_stalled;
    if (stalls != lastStalls) {
        lastStalls = stalls;
        lastStallUs = nowUs;
    }
    const bool linkUp = stats.locked && (lastStallUs == 0 || nowUs - lastStallUs > 1'000'000);

    // The newest activity's color, writes over reads over the rest.
    if (pending != 0) {
        auto has = [](LedActivity a) { return (pending & (1u << static_cast<unsigned>(a))) != 0; };
        if (has(LedActivity::Write)) {
            flashR = 255, flashG = 80, flashB = 0;
        } else if (has(LedActivity::Read)) {
            flashR = 0, flashG = 255, flashB = 0;
        } else if (has(LedActivity::Transfer)) {
            flashR = 255, flashG = 0, flashB = 255;
        } else {
            flashR = 0, flashG = 200, flashB = 255;
        }
        flashLevel = kFlashLevel;
        pending = 0;
    }

    // Rest color under the flash (kept dim: the LED is bright).
    int r = linkUp ? 0 : 8, g = 0, b = linkUp ? 6 : 0;
    if (flashLevel > 0) {
        // Scaled to 1/8: full brightness is uncomfortable to look at.
        r += flashR * flashLevel / 255 / 8;
        g += flashG * flashLevel / 255 / 8;
        b += flashB * flashLevel / 255 / 8;
        flashLevel -= kDecayPerFrame;
    }
    frame[0] = static_cast<uint8_t>(g > 255 ? 255 : g);
    frame[1] = static_cast<uint8_t>(r > 255 ? 255 : r);
    frame[2] = static_cast<uint8_t>(b > 255 ? 255 : b);
    if (frame[0] == shown[0] && frame[1] == shown[1] && frame[2] == shown[2]) return;

    // Queued, not waited for: the previous 30 us frame finished long ago
    // (frames are 20 ms apart); the line idles low between them, which is
    // the chip's latch.
    rmt_transmit_config_t tx = {};
    if (rmt_transmit(channel, encoder, frame, sizeof(frame), &tx) == ESP_OK) {
        shown[0] = frame[0], shown[1] = frame[1], shown[2] = frame[2];
    }
}
