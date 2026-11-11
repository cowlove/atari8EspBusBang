#include "bus_profiler.h"

#include <stdio.h>

#include "driver/dedic_gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool Hist2::binsFull = false;   // DRAM_ATTR comes from the declaration

uint32_t busProfilerInitBundle(const BusProfilerPins &pins) {
    // Inputs only: the bundle adds GPIO-matrix input routes and leaves
    // whatever drives a pin alone (in host Sidecar builds, GPIO48 is the
    // link's BCLK and GPIO21 its RX; i2slink keeps BCLK's input enabled).
    int gpioArray[8] = {pins.clock, pins.profile, pins.rw, pins.extSel,
                        pins.d0, pins.d1, pins.a0, pins.refresh};
    const int count = pins.refresh >= 0 ? 8 : 7;
    printf("bus_profiler: configuring dedic_gpio_bundle for %d inputs\n", count);
    const dedic_gpio_bundle_config_t config = {
        .gpio_array = gpioArray,
        .array_size = static_cast<size_t>(count),
        .flags = {.in_en = 1, .in_invert = 0, .out_en = 0, .out_invert = 0},
    };
    dedic_gpio_bundle_handle_t bundle = nullptr;
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&config, &bundle));
    uint32_t offset = 0;
    ESP_ERROR_CHECK(dedic_gpio_get_in_offset(bundle, &offset));
    return offset;
}

void busProfilerPrintHistograms(const Hist2 *hist, int channels) {
    printf("ticks   profile_pin   extSel_wr    extsel_rd    rd_extSel     rd_normal    wr_extSel    wr_normal    addr_all        rw_          clock\n");
    int first = hist[0].maxBucket, last = 0;
    for (int c = 0; c < channels; ++c) {
        for (int i = 1; i < hist[c].maxBucket; ++i) {
            if (hist[c].buckets[i] > 0 && i > last) last = i;
        }
        for (int i = hist[c].maxBucket - 1; i > 0; --i) {
            if (hist[c].buckets[i] > 0 && i < first) first = i;
        }
    }
    for (int i = first; i <= last; ++i) {
        printf("% 4d ", i * 2 /*i is burst sample iteration counts, about 2 cycles*/);
        for (int c = 0; c < channels; ++c)
            printf("% 12d ", static_cast<int>(hist[c].buckets[i]));
        printf(" HIST\n");
    }

    for (int c = 0; c < channels; ++c) {
        first = last = 0;
        int total = 0;
        for (int i = 1; i < hist[c].maxBucket; ++i) {
            if (hist[c].buckets[i] > 0) last = i;
            total += hist[c].buckets[i];
        }
        for (int i = hist[c].maxBucket - 1; i > 0; --i) {
            if (hist[c].buckets[i] > 0) first = i;
        }
        taskYIELD();
        printf("channel %d: range %3d -%3d, jitter %3d, total %d  HIST\n", c, first, last, last - first, total);
    }
    uint64_t histogramSamples = 0;
    for (int i = 0; i < hist[0].maxBucket; ++i)
        histogramSamples += hist[0].buckets[i];
    printf("Total samples %" PRIu64 " implies %.2f sec sampling\n", histogramSamples,
        1.0 * histogramSamples / 1.8 / 1000000);
}

void busProfilerPrintCaptures(const BusProfilerState &state, const BusProfilerPins &pins,
                              bool profilePinOutput) {
    printf("GPIO sampler: offset %" PRIu32 ", elapsed %" PRIu32 " cycles, %" PRIu32 " captures, %" PRIu32 " without clock fall, profile edges %" PRIu32 "\n",
        state.result.inputOffset, state.result.elapsedCycles,
        state.result.captures, state.result.noClockFall,
        state.result.profileTransitions);
    printf("Input bundle bits: 0=clock GPIO%d, 1=profile GPIO%d\n", pins.clock, pins.profile);
    if (!profilePinOutput) {
        printf("  2=R/W GPIO%d, 3=EXTSEL GPIO%d, 4=D0 GPIO%d, 5=D1 GPIO%d, 6=A0 GPIO%d",
            pins.rw, pins.extSel, pins.d0, pins.d1, pins.a0);
        if (pins.refresh >= 0)
            printf(", 7=refresh_ GPIO%d", pins.refresh);
        printf("\n");
        printf("Histogram channels: 0=profile pin; 1/2=EXTSEL edges on writes/reads; "
               "3/4=D0-D1 edges on reads with EXTSEL low/high; 5/6=the same on writes; "
               "7=A0; 8=R/W; 9=clock\n");
    } else {
        printf("Histogram channels: 0=first profile-pin rising edge after clock fall\n");
    }
    const uint8_t *samples = state.hasValidSamples ? state.validSamples : state.samples;
    if (state.hasValidSamples) {
        printf("Last GPIO snapshot with an edge (clock fall=%" PRIu32 ", last edge sample=%" PRIu32 "):",
            state.validFallSample, state.validEventSample);
    } else {
        printf("No qualified GPIO snapshot; final burst samples:");
    }
    for (int i = 0; i < busProfilerReads; ++i) {
        if (i % 15 == 0) printf("\n%02d:", i);
        printf(" %02" PRIx8, samples[i]);
    }
    printf("\n");
}
