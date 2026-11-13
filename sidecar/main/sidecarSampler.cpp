// The Sidecar's bus sampler: see sidecarSampler.h.
//
// A task on core 0 (the service loop is pinned to core 1) owns a dedicated-
// GPIO input bundle of the eight bus pins and runs bus_profiler's capture in
// short chunks with interrupts off, yielding between them, so a flash write
// on core 1 (SPIFFS, which needs core 0 briefly) waits at most one chunk.
// It works out the bus clock's rate first (a real 6502's ~1.78 MHz or the
// main board's FAKE_CLOCK bench clock) and uses the matching capture.

#include "sidecarSampler.h"

#include "bus_profiler.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>

namespace {

// Bundle bit order is fixed by bus_profiler (BusProfilerPins). Sidecar
// GPIOs, from the main board's: phi2 36 -> 4, R/W 17 -> 5, extSel_ 47 -> 6,
// D0 38 -> 7, D1 39 -> 15, A0 1 -> 16, refresh_ (the Atari's RFSH, 3.3 V
// side) -> 17, on an internal pull-up so an unwired bench pair reads every
// cycle as a real one. The profile-pin slot is GPIO10, the link's BCLK: an
// extra input route on it doesn't disturb the link, and its edges aren't
// recorded.
constexpr BusProfilerPins kPins = {4, 10, 5, 6, 7, 15, 16, 17};
constexpr int kRefreshGpio = 17;
// extSel_ is open drain on the main board (driven low, else released): the
// Atari's pull-up brings it back. On a bench pair with no Atari nothing
// does, so a weak internal pull-up here; harmless beside the Atari's.
constexpr int kExtSelGpio = 6;
constexpr const char *kBitNames[8] = {"phi2", "profile/BCLK", "R/W", "extSel_",
                                      "D0", "D1", "A0", "refresh_"};

constexpr uint32_t kCpuHz = 240 * 1000000;
// Half a bus clock period in CPU cycles, per clock: a 6502 at 1.78 MHz, and
// the main board's FAKE_CLOCK (APB 80 MHz / 56, main/fakeBus.h).
constexpr uint32_t kRealHalfTicks = kCpuHz / 1780000 / 2;          // 67
constexpr uint32_t kBenchHalfTicks = kCpuHz / (80000000 / 56) / 2;  // 84
constexpr uint32_t kChunkCycles = kCpuHz / 1000 * 2;                // 2 ms
constexpr int64_t kReportUs = 10 * 1000000;

DRAM_ATTR Hist2 hist[busProfilerChannels];
DRAM_ATTR BusProfilerState state = {};
uint32_t inOffset = 0;
volatile bool reportRequested = false;

// Which bundle bits toggle, and the phi2 rate, over a stretch of back-to-
// back reads. The loop only ORs changes and counts clock falls, so it keeps
// up with every edge (per-bit counting fell behind and undercounted phi2).
struct PinSurvey {
    uint32_t active;      // bundle bits that changed at least once
    uint32_t clockFalls;
    uint32_t cycles;
};

IRAM_ATTR PinSurvey surveyPins(uint32_t cycles)
{
    PinSurvey s = {};
    const uint32_t clockMask = 1u << inOffset;
    const uint32_t start = XTHAL_GET_CCOUNT();
    uint32_t last = dedic_gpio_cpu_ll_read_in();
    uint32_t active = 0, falls = 0;
    while (static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start) < cycles) {
        const uint32_t now = dedic_gpio_cpu_ll_read_in();
        active |= now ^ last;
        falls += (last & ~now & clockMask) != 0;
        last = now;
    }
    s.active = (active >> inOffset) & 0xff;
    s.clockFalls = falls;
    s.cycles = static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start);
    return s;
}

void printSurvey(const PinSurvey &s)
{
    printf("sampler: pin survey over %" PRIu32 " us: active", s.cycles / (kCpuHz / 1000000));
    for (int b = 0; b < 8; ++b)
        if (s.active & (1u << b)) printf(" %s", kBitNames[b]);
    printf("; idle");
    for (int b = 0; b < 8; ++b)
        if (!(s.active & (1u << b))) printf(" %s", kBitNames[b]);
    if (s.clockFalls)
        printf("; phi2 %.3f MHz\n", 1.0 * s.clockFalls * kCpuHz / s.cycles / 1e6);
    else
        printf("; no phi2\n");
}

// One chunk, interrupts off on this core. The capture is always_inline, so
// each clock gets its own IRAM copy.
template <uint32_t HalfTicks>
IRAM_ATTR void captureChunk()
{
    portDISABLE_INTERRUPTS();
    // Bounded waits: the main board's clock stops whenever it's reset or
    // flashed, and an unbounded wait with interrupts off froze core 0 (a
    // flash access from core 1 then ended in a cache-error panic).
    busProfilerCaptureForCycles<true, true, false, HalfTicks, true>(state, hist, inOffset, kChunkCycles);
    portENABLE_INTERRUPTS();
}

void report(uint64_t captures, uint64_t noFall, double mhz)
{
    printf("sampler: %.3f MHz phi2, %" PRIu64 " captures (%" PRIu64 " without a clock fall)\n",
           mhz, captures, noFall);
    busProfilerPrintHistograms(hist, busProfilerChannels);
    busProfilerPrintCaptures(state, kPins, false);
    for (auto &h : hist)
        h.clear();
}

void samplerTask(void *)
{
    // The bundle belongs to the core that creates it: this one.
    gpio_pullup_en(static_cast<gpio_num_t>(kRefreshGpio));
    gpio_pullup_en(static_cast<gpio_num_t>(kExtSelGpio));
    inOffset = busProfilerInitBundle(kPins);
    for (;;) {
        // Find the clock (or wait for one: the main board may be in reset,
        // being flashed, or not running a bus).
        const PinSurvey s = surveyPins(kCpuHz / 1000 * 20);   // 20 ms
        printSurvey(s);
        if (s.clockFalls < 100) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        const double mhz = 1.0 * s.clockFalls * kCpuHz / s.cycles / 1e6;
        const bool bench = mhz < 1.6;
        printf("sampler: sampling a %s clock\n", bench ? "FAKE_CLOCK bench" : "6502");
        uint64_t captures = 0, noFall = 0;
        int64_t nextReport = esp_timer_get_time() + kReportUs;
        for (;;) {
            if (bench)
                captureChunk<kBenchHalfTicks>();
            else
                captureChunk<kRealHalfTicks>();
            captures += state.result.captures;
            noFall += state.result.noClockFall;
            vTaskDelay(1);   // let the idle task (and its watchdog) run
            const int64_t now = esp_timer_get_time();
            if (reportRequested || now >= nextReport) {
                reportRequested = false;
                nextReport = now + kReportUs;
                report(captures, noFall, mhz);
                // The clock stopped (or changed rate): survey again.
                if (state.result.captures == 0 || noFall * 2 > captures)
                    break;
                captures = noFall = 0;
            }
        }
    }
}

}  // namespace

void sidecarSamplerBegin()
{
    xTaskCreatePinnedToCore(samplerTask, "sampler", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr, 0);
}

void sidecarSamplerRequestReport()
{
    reportRequested = true;
}
