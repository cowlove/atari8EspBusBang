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
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <algorithm>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

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

// In chunks with interrupts off (kChunkCycles, as the captures), counting
// only the time actually sampled: with interrupts on, a preemption (core
// 0's WiFi and lwIP tasks, busiest just after boot, and the tick) hid the
// falls under it while its time still counted, so phi2 read low: the
// FAKE_CLOCK's 1.4286 MHz as 0.95-1.0 MHz in the first survey after the
// Sidecar's boot, 1.424 later.
IRAM_ATTR void surveyChunk(PinSurvey &s, uint32_t cycles)
{
    const uint32_t clockMask = 1u << inOffset;
    portDISABLE_INTERRUPTS();
    const uint32_t start = XTHAL_GET_CCOUNT();
    uint32_t last = dedic_gpio_cpu_ll_read_in();
    uint32_t active = 0, falls = 0;
    while (static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start) < cycles) {
        const uint32_t now = dedic_gpio_cpu_ll_read_in();
        active |= now ^ last;
        falls += (last & ~now & clockMask) != 0;
        last = now;
    }
    const uint32_t elapsed = static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start);
    portENABLE_INTERRUPTS();
    s.active |= (active >> inOffset) & 0xff;
    s.clockFalls += falls;
    s.cycles += elapsed;
}

PinSurvey surveyPins(uint32_t cycles)
{
    PinSurvey s = {};
    while (s.cycles < cycles) {
        surveyChunk(s, kChunkCycles);
        vTaskDelay(1);   // let the idle task (and its watchdog) run
    }
    return s;
}

void printSurvey(const PinSurvey &s)
{
    char active[96] = "", idle[96] = "";
    for (int b = 0; b < 8; ++b) {
        char *list = (s.active & (1u << b)) ? active : idle;
        strlcat(list, " ", sizeof(active));
        strlcat(list, kBitNames[b], sizeof(active));
    }
    if (s.clockFalls)
        printf("sampler: pin survey over %" PRIu32 " us: active%s; idle%s; phi2 %.3f MHz\n",
               s.cycles / (kCpuHz / 1000000), active, idle, 1.0 * s.clockFalls * kCpuHz / s.cycles / 1e6);
    else
        printf("sampler: pin survey over %" PRIu32 " us: active%s; idle%s; no phi2\n",
               s.cycles / (kCpuHz / 1000000), active, idle);
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

// Sample periods (SystemControl from main, or the console's "sample"): the
// histograms gather over one period, a category main names; a new Start,
// a category change or a Stop ends it with a final report. Written by the
// service task, read here.
std::atomic<bool> enabled{true};          // sampling (until a Stop)
std::atomic<uint8_t> wantCategory{0};
std::atomic<uint32_t> controlGen{0};      // bumped by every Start and Stop
std::atomic<uint32_t> periodsReported{0};
std::atomic<bool> capturing{false};       // in the capture loop (a clock found)
std::atomic<uint32_t> timelineMs{0};      // a timeline asked for (sidecarSamplerTimeline())
std::atomic<uint32_t> timelineBucketMs{25};
// Labeled moments on the running timeline (sidecarSamplerTimelineMark()):
// the EN reset, main restarting itself, main's boot report.
constexpr int kTimelineMarks = 4;
std::atomic<int64_t> timelineMarkUs[kTimelineMarks];
const char *timelineMarkLabel[kTimelineMarks];
std::atomic<int> timelineMarks{0};


struct Period {
    uint32_t number = 0;
    uint8_t category = 0;
    uint64_t captures = 0, noFall = 0;
    int64_t startUs = 0;
    double mhz = 0;   // the clock it last sampled (0: none found)
};
Period period;

void report(const Period &p, bool final)
{
    const double mhz = p.mhz;
    const int64_t ms = (esp_timer_get_time() - p.startUs) / 1000;
    printf("sampler: period %" PRIu32 " (category %u, %s, %lld ms): %.3f MHz phi2, %" PRIu64
           " captures (%" PRIu64 " without a clock fall)\n",
           p.number, (unsigned)p.category, final ? "ended" : "so far", (long long)ms, mhz, p.captures, p.noFall);
    if (p.captures) {
        busProfilerPrintHistograms(hist, busProfilerChannels);
        busProfilerPrintCaptures(state, kPins, false);
    }
}

// Per-category totals: every ended period of a category added up (its
// histograms, captures, time), across periods and runs until cleared.
// Categories 0..kTotalCategories-1; a set (~7.7 KB) is allocated on its
// first period, in PSRAM if there is any. Only this task touches them; the
// console and main ask through totalsRequest/clearRequest.
constexpr int kTotalCategories = 16;
struct Totals {
    Hist2 *hist = nullptr;   // busProfilerChannels sets
    uint32_t periods = 0;
    uint64_t captures = 0, noFall = 0;
    int64_t ms = 0;
    double mhz = 0;   // the last period's clock
};
Totals totals[kTotalCategories];
constexpr int kAllCategories = 255;
std::atomic<int> totalsRequest{-1};   // a category, kAllCategories, or -1 none
std::atomic<int> clearRequest{-1};

void addToTotals(const Period &p)
{
    if (p.category >= kTotalCategories) return;   // not kept
    Totals &t = totals[p.category];
    if (!t.hist) {
        t.hist = static_cast<Hist2 *>(heap_caps_calloc(busProfilerChannels, sizeof(Hist2),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!t.hist)
            t.hist = static_cast<Hist2 *>(heap_caps_calloc(busProfilerChannels, sizeof(Hist2), MALLOC_CAP_8BIT));
    }
    if (t.hist) {
        for (int c = 0; c < busProfilerChannels; ++c)
            for (int b = 0; b < Hist2::maxBucket; ++b) {
                const uint64_t sum = uint64_t(t.hist[c].buckets[b]) + hist[c].buckets[b];
                t.hist[c].buckets[b] = sum > 0xffffffffu ? 0xffffffffu : uint32_t(sum);
            }
    }
    t.periods++;
    t.captures += p.captures;
    t.noFall += p.noFall;
    t.ms += (esp_timer_get_time() - p.startUs) / 1000;
    if (p.mhz > 0) t.mhz = p.mhz;
}

void printTotalsLine(int c)
{
    const Totals &t = totals[c];
    printf("sampler: category %d totals: %" PRIu32 " periods, %lld ms, %.3f MHz phi2, %" PRIu64
           " captures (%" PRIu64 " without a clock fall)%s\n", c, t.periods, (long long)t.ms, t.mhz, t.captures,
           t.noFall, t.hist || !t.periods ? "" : " (no memory for its histograms)");
}

// One category's totals, histograms included, or every category's that has
// periods (onlyMulti: only those with two or more: one period's totals are
// its own report).
void printTotals(int which, bool onlyMulti)
{
    for (int c = 0; c < kTotalCategories; ++c) {
        if ((which != kAllCategories && c != which) || totals[c].periods == 0) continue;
        if (onlyMulti && totals[c].periods < 2) continue;
        printTotalsLine(c);
        if (totals[c].hist && totals[c].captures)
            busProfilerPrintHistograms(totals[c].hist, busProfilerChannels);
    }
}

void clearTotals(int which)
{
    for (int c = 0; c < kTotalCategories; ++c) {
        if (which != kAllCategories && c != which) continue;
        Totals &t = totals[c];
        if (t.hist)
            for (int h = 0; h < busProfilerChannels; ++h) t.hist[h].clear();
        t.periods = 0;
        t.captures = t.noFall = 0;
        t.ms = 0;
        t.mhz = 0;
    }
}

// The two cores' handshake (each stores its flag, then reads the other's):
// sequentially consistent, so neither read can pass the other core's store.
std::atomic<bool> quiet{false};
std::atomic<bool> printing{false};   // a survey or report is going out

// Print through the quiet handshake (see sidecarSamplerQuiet()). Each
// line goes out in one printf(), so other tasks' output (main's console
// text, printMainOut() in sidecarApp.cpp) can't land inside it.
template <typename F> void say(F &&f)
{
    printing = true;   // before checking quiet
    if (!quiet)
        f();
    printing = false;
}

// A period's end: its report, then into its category's totals, the
// category's running total, at a Stop every category's totals (those with
// more than one period: histograms), and last the token for scripts
// (boot_sidecar.sh reads on after main's DONE to the "stop" one, main's
// last control): "next" when another period follows, "stop" when sampling
// pauses. The token comes after the totals so a capture that stops at it
// has them.
void endPeriod()
{
    const bool stopping = !enabled;
    say([&] {
        report(period, true);
        addToTotals(period);
        // The category's running total (at a Stop the totals below print
        // it, for a category with two or more periods).
        if (period.category < kTotalCategories && !(stopping && totals[period.category].periods >= 2))
            printTotalsLine(period.category);
        if (stopping) printTotals(kAllCategories, true);
        printf("SAMPLER_PERIOD_END %" PRIu32 " category %u captures %" PRIu64 " %s\n", period.number,
               (unsigned)period.category, period.captures, stopping ? "stop" : "next");
    });
    for (auto &h : hist)
        h.clear();
    periodsReported.fetch_add(1);
}

// The console's and main's totals requests (print, clear), done here.
void serviceRequests()
{
    const int show = totalsRequest.exchange(-1);
    if (show >= 0) {
        say([&] {
            bool any = false;
            for (int c = 0; c < kTotalCategories; ++c)
                any |= (show == kAllCategories || c == show) && totals[c].periods;
            if (any)
                printTotals(show, false);
            else
                printf("sampler: no totals yet%s\n", show == kAllCategories ? "" : " for that category");
        });
    }
    const int clear = clearRequest.exchange(-1);
    if (clear >= 0) {
        clearTotals(clear);
        say([&] { printf("sampler: totals cleared (%s)\n", clear == kAllCategories ? "all" : "one category"); });
    }
}

// A new period: the category asked for, counters from zero.
void beginPeriod()
{
    period.number++;
    period.category = wantCategory.load();
    period.captures = period.noFall = 0;
    period.startUs = esp_timer_get_time();
    period.mhz = 0;
}

// Wait up to ms for a control (a Start/Stop): true if one came.
bool waitForControl(uint32_t gen, uint32_t ms)
{
    for (uint32_t waited = 0; waited < ms; waited += 100) {
        if (controlGen.load() != gen) return true;
        serviceRequests();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

// The timeline: per bucket, what the sampled time held. R/W and extSel_ as
// they were just before each phi2 fall (the cycle's end).
constexpr int kTimelineMaxBuckets = 400;
struct TimelineBucket { uint32_t sampledCycles, falls, writes, extSel; int64_t firstWriteUs; };   // firstWriteUs 0: none
TimelineBucket timeline[kTimelineMaxBuckets];

IRAM_ATTR void timelineChunk(TimelineBucket &b, uint32_t cycles)
{
    const uint32_t clockMask = 1u << inOffset, rwMask = 4u << inOffset, extSelMask = 8u << inOffset;
    portDISABLE_INTERRUPTS();
    const int64_t startUs = esp_timer_get_time();
    const uint32_t start = XTHAL_GET_CCOUNT();
    uint32_t last = dedic_gpio_cpu_ll_read_in();
    uint32_t falls = 0, writes = 0, ext = 0, firstWrite = 0;
    bool wrote = false;
    while (static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start) < cycles) {
        const uint32_t now = dedic_gpio_cpu_ll_read_in();
        if (last & ~now & clockMask) {
            falls++;
            if ((last & rwMask) == 0) {
                if (!wrote) { wrote = true; firstWrite = XTHAL_GET_CCOUNT(); }
                writes++;
            }
            ext += (last & extSelMask) == 0;
        }
        last = now;
    }
    const uint32_t elapsed = static_cast<uint32_t>(XTHAL_GET_CCOUNT() - start);
    portENABLE_INTERRUPTS();
    if (wrote && b.firstWriteUs == 0)
        b.firstWriteUs = startUs + static_cast<uint32_t>(firstWrite - start) / (kCpuHz / 1000000);
    b.sampledCycles += elapsed;
    b.falls += falls;
    b.writes += writes;
    b.extSel += ext;
}

void runTimeline(uint32_t ms, int bucketMs)
{
    const int buckets = std::min<int>(kTimelineMaxBuckets, (ms + bucketMs - 1) / bucketMs);
    const int chunks = bucketMs >= 10 ? 5 : 1;
    memset(timeline, 0, sizeof(timeline));
    timelineMarks = 0;
    const int64_t t0 = esp_timer_get_time();
    int64_t lastDelayUs = t0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        const int i = static_cast<int>((now - t0) / (bucketMs * 1000));
        if (i >= buckets) break;
        for (int k = 0; k < chunks; k++) timelineChunk(timeline[i], kChunkCycles);   // 2 ms each, interrupts on between
        // A tick is 10 ms: fine buckets sample back to back (every other
        // task outranks this one and runs between chunks), yielding to the
        // idle task once per 200 ms only.
        if (chunks > 1 || now - lastDelayUs >= 200000) {
            vTaskDelay(1);
            lastDelayUs = esp_timer_get_time();
        }
    }
    const int marks = std::min(timelineMarks.load(), kTimelineMarks);
    say([&] {
        printf("REBOOT_TIMELINE: %d ms buckets from the timeline's start; per sampled ms: phi2 falls, writes, "
               "extSel_ low; the bucket's first write;", bucketMs);
        for (int m = 0; m < marks; m++)
            printf(" %s at %+lld ms", timelineMarkLabel[m], (long long)((timelineMarkUs[m].load() - t0) / 1000));
        printf("\n");
        for (int i = 0; i < buckets; i++) {
            const TimelineBucket &b = timeline[i];
            const double sampledMs = b.sampledCycles / (kCpuHz / 1000.0);
            if (sampledMs <= 0) { printf("REBOOT_TIMELINE %+5d ms: not sampled\n", i * bucketMs); continue; }
            printf("REBOOT_TIMELINE %+5d ms: phi2 %6.0f wr %5.0f extsel %5.0f (%.0f ms sampled)",
                   i * bucketMs, b.falls / sampledMs, b.writes / sampledMs, b.extSel / sampledMs, sampledMs);
            if (b.firstWriteUs) printf(" first wr %+.1f ms", (b.firstWriteUs - t0) / 1000.0);
            for (int m = 0; m < marks; m++)
                if ((timelineMarkUs[m].load() - t0) / 1000 / bucketMs == i)
                    printf("  <- %s", timelineMarkLabel[m]);
            printf("\n");
        }
        fflush(stdout);
    });
}

void samplerTask(void *)
{
    // The bundle belongs to the core that creates it: this one.
    gpio_pullup_en(static_cast<gpio_num_t>(kRefreshGpio));
    gpio_pullup_en(static_cast<gpio_num_t>(kExtSelGpio));
    inOffset = busProfilerInitBundle(kPins);
    beginPeriod();
    for (;;) {
        uint32_t gen = controlGen.load();
        if (!enabled) {
            waitForControl(gen, 3600000);   // paused until a Start
            if (enabled) beginPeriod();
            continue;
        }
        // Find the clock (or wait for one: the main board may be in reset,
        // being flashed, or not running a bus).
        const PinSurvey s = surveyPins(kCpuHz / 1000 * 20);   // 20 ms
        say([&] { printSurvey(s); });
        if (s.clockFalls < 100) {
            // 20 s (quiet soak logs; a clock is found within 20 s), or until
            // a control: a Start comes as main's bus starts.
            if (waitForControl(gen, 20000)) {
                endPeriod();
                if (enabled) beginPeriod();
            }
            continue;
        }
        const double mhz = 1.0 * s.clockFalls * kCpuHz / s.cycles / 1e6;
        const bool bench = mhz < 1.6;
        period.mhz = mhz;
        say([&] { printf("sampler: sampling a %s clock (period %" PRIu32 ", category %u)\n",
                         bench ? "FAKE_CLOCK bench" : "6502", period.number, (unsigned)period.category); });
        uint64_t sinceReport = 0, noFallSinceReport = 0;
        int64_t nextReport = esp_timer_get_time() + kReportUs;
        capturing = true;
        for (;;) {
            if (bench)
                captureChunk<kBenchHalfTicks>();
            else
                captureChunk<kRealHalfTicks>();
            period.captures += state.result.captures;
            period.noFall += state.result.noClockFall;
            sinceReport += state.result.captures;
            noFallSinceReport += state.result.noClockFall;
            vTaskDelay(1);   // let the idle task (and its watchdog) run
            serviceRequests();
            if (const uint32_t ms = timelineMs.exchange(0)) {
                runTimeline(ms, static_cast<int>(timelineBucketMs.load()));
                nextReport = esp_timer_get_time() + kReportUs;   // its time isn't a stopped clock
                sinceReport = noFallSinceReport = 0;
            }
            if (controlGen.load() != gen) {
                // A Start (new period or category) or a Stop: this period ends.
                gen = controlGen.load();
                endPeriod();
                if (!enabled) { capturing = false; break; }   // paused: the outer loop waits
                beginPeriod();
                period.mhz = mhz;
                say([&] { printf("sampler: period %" PRIu32 " (category %u) begins\n", period.number,
                                 (unsigned)period.category); });
                sinceReport = noFallSinceReport = 0;
                nextReport = esp_timer_get_time() + kReportUs;
                continue;
            }
            const int64_t now = esp_timer_get_time();
            // A report only when asked (the console's "sample report", main's
            // Report); the histograms otherwise print once, at the period's
            // end (main's Stop before its exit report: the totals too).
            if (!quiet && reportRequested) {
                reportRequested = false;
                say([&] { report(period, false); });
            }
            // Every 10 s: the clock stopped (or changed rate)? Survey again.
            if (now >= nextReport) {
                nextReport = now + kReportUs;
                if (state.result.captures == 0 || noFallSinceReport * 2 > sinceReport) {
                    capturing = false;
                    break;
                }
                sinceReport = noFallSinceReport = 0;
            }
        }
    }
}

}  // namespace

// Quiet waits out a print already going (bounded: 0.5 s).
void sidecarSamplerQuiet(bool on)
{
    quiet = on;
    for (int i = 0; on && printing && i < 500; ++i)
        vTaskDelay(pdMS_TO_TICKS(1));
}

void sidecarSamplerBegin()
{
    xTaskCreatePinnedToCore(samplerTask, "sampler", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr, 0);
}

void sidecarSamplerRequestReport()
{
    reportRequested = true;
}

uint32_t sidecarSamplerControl(SidecarSamplerAction action, uint8_t category)
{
    switch (action) {
    case SidecarSamplerAction::Stop:
        enabled = false;
        controlGen.fetch_add(1);
        break;
    case SidecarSamplerAction::Start:
        wantCategory = category;
        enabled = true;
        controlGen.fetch_add(1);
        break;
    case SidecarSamplerAction::Report:
        reportRequested = true;
        break;
    case SidecarSamplerAction::Totals:
        totalsRequest = category;
        break;
    case SidecarSamplerAction::ClearTotals:
        clearRequest = category;
        break;
    }
    return periodsReported.load();
}

bool sidecarSamplerTimeline(uint32_t ms, uint32_t bucketMs)
{
    if (!capturing) return false;
    timelineBucketMs = bucketMs < 1 ? 1 : bucketMs;
    timelineMs = ms;
    return true;
}

void sidecarSamplerTimelineMark(const char *label)
{
    const int m = timelineMarks.load();
    if (m >= kTimelineMarks) return;
    timelineMarkLabel[m] = label;
    timelineMarkUs[m] = esp_timer_get_time();
    timelineMarks = m + 1;
}

void sidecarSamplerStatus()
{
    printf("Sidecar: sampler %s, period %" PRIu32 " (category %u, %" PRIu64 " captures), %" PRIu32
           " periods reported\n", enabled ? "sampling" : "stopped", period.number, (unsigned)period.category,
           period.captures, periodsReported.load());
}
