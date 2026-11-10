// FAKE_CLOCK bench bus: drives a synthetic 6502 clock, R/W and A0 on an
// unattached board, and maps the test page, so core 1's loop and the
// profiler run with no Atari attached.
#ifdef FAKE_CLOCK
#include <stdio.h>
#include <inttypes.h>
#include "driver/ledc.h"
#include "esp_err.h"
#include "soc/ledc_struct.h"
#include "soc/ledc_reg.h"
#include "arduinoLite.h"
#include "pinDefs.h"
#include "mmu.h"
#include "fakeBus.h"

// Clock, R/W and A0 for FAKE_CLOCK, phase-locked. Each LEDC timer runs off
// the APB clock with the same divider (fakeClockDiv, exactly 7.0: the
// driver rounds the divider to 1/256, and these freq_hz values land on it),
// so one count is 7 APB ticks (87.5 ns, 21 CPU cycles) and the three never
// drift. The clock is high for counts 0-3 and falls at count 4. A0 (4
// clock periods) and R/W (8) change at count 3, one count before a clock
// fall: after core 1's last sample of the cycle (its write sample and
// release, ~75 cycles + APB latency after the fall) and before its next
// address read and the profiler's R/W look, so both see the same, settled
// R/W and address for every cycle.
// The timers are held in reset and released with back-to-back register
// stores, so they start within an APB tick or two of each other (the
// driver's pause/reset/resume calls left A0 ~21 and R/W ~39 CPU cycles
// behind the clock).
static void fakeBusSignalsStart() {
    struct Sig { int pin; ledc_timer_t timer; ledc_channel_t ch; int resBits; uint32_t duty, hpoint; };
    // Aim two counts before the fall: the measured lead leaves A0 ~1 count
    // and R/W ~0.5 count later than aimed (repeatable across boots), and
    // this keeps both clear of core 1's address read (~22 cycles after the
    // fall) and the profiler's R/W look (~80).
    static constexpr uint32_t edge = fakeClockCounts / 2 - 2;
    const Sig sigs[] = {
        {bus.clock.pin, LEDC_TIMER_0, LEDC_CHANNEL_0, 3, fakeClockCounts / 2, 0},        // 8 counts
        {bus.addr.pin,  LEDC_TIMER_2, LEDC_CHANNEL_2, 5, 2 * fakeClockCounts, edge},     // 32 = 4 periods
        {bus.rw.pin,    LEDC_TIMER_1, LEDC_CHANNEL_1, 6, 4 * fakeClockCounts, edge},     // 64 = 8 periods
    };
    static_assert(fakeClockCounts == 8, "resBits above assume 8 counts per clock period");
    for (const Sig &g : sigs) {
        ledc_timer_config_t t = {};
        t.speed_mode = LEDC_LOW_SPEED_MODE;
        t.duty_resolution = (ledc_timer_bit_t)g.resBits;
        t.timer_num = g.timer;
        t.freq_hz = fakeClockApbHz / (fakeClockDiv << g.resBits);
        t.clk_cfg = LEDC_USE_APB_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&t));
        ledc_channel_config_t c = {};
        c.gpio_num = g.pin;
        c.speed_mode = LEDC_LOW_SPEED_MODE;
        c.channel = g.ch;
        c.timer_sel = g.timer;
        c.duty = g.duty;
        c.hpoint = g.hpoint;
        ESP_ERROR_CHECK(ledc_channel_config(&c));
    }
    // Hold all three in reset, then release them together.
    volatile uint32_t *conf[3];
    uint32_t run[3];
    for (int i = 0; i < 3; i++) {
        conf[i] = &LEDC.timer_group[LEDC_LOW_SPEED_MODE].timer[sigs[i].timer].conf.val;
        *conf[i] |= LEDC_LSTIMER0_RST;
        // PARA_UP (self-clearing) latches a low-speed timer's conf change.
        run[i] = (*conf[i] & ~(uint32_t)(LEDC_LSTIMER0_RST | LEDC_LSTIMER0_PAUSE))
               | LEDC_LSTIMER0_PARA_UP;
    }
    *conf[0] = run[0];
    *conf[1] = run[1];
    *conf[2] = run[2];
    // The release still leaves the timers a few counts apart (measured: A0's
    // edge 48, R/W's 110 CPU cycles after the clock fall). They don't drift,
    // so measure the phase and set each hpoint so its edge lands at count
    // `edge` of the clock's period. The counters tick every 7 APB cycles,
    // faster than a few register reads, so freeze them to read: pause with
    // back-to-back stores, read, resume with the same stores in the same
    // order (the stagger cancels).
    volatile uint32_t *cnt[3];
    uint32_t paused[3];
    for (int i = 0; i < 3; i++) {
        cnt[i] = &LEDC.timer_group[LEDC_LOW_SPEED_MODE].timer[sigs[i].timer].value.val;
        paused[i] = run[i] | LEDC_LSTIMER0_PAUSE;
    }
    *conf[0] = paused[0];
    *conf[1] = paused[1];
    *conf[2] = paused[2];
    const uint32_t c0 = *cnt[0] & 0x3fff;
    const uint32_t c1 = *cnt[1] & 0x3fff;
    const uint32_t c2 = *cnt[2] & 0x3fff;
    *conf[0] = run[0];
    *conf[1] = run[1];
    *conf[2] = run[2];
    const uint32_t lead[3] = {0, (c1 - c0) % fakeClockCounts, (c2 - c0) % fakeClockCounts};
    for (int i = 1; i < 3; i++) {
        const uint32_t hpoint = (edge + lead[i]) % fakeClockCounts;
        ESP_ERROR_CHECK(ledc_set_duty_with_hpoint(LEDC_LOW_SPEED_MODE, sigs[i].ch, sigs[i].duty, hpoint));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, sigs[i].ch));
    }
    for (int i = 0; i < 3; i++)
        printf("fake bus: GPIO%d timer %d %" PRIu32 " Hz, counter lead %" PRIu32 " (hpoint %" PRIu32 ")\n",
               sigs[i].pin, (int)sigs[i].timer, ledc_get_freq(LEDC_LOW_SPEED_MODE, sigs[i].timer),
               lead[i], i ? (edge + lead[i]) % fakeClockCounts : 0);
}


void setupFakeBusInjection() {
    pinMode(bus.clock.pin, OUTPUT);
    digitalWrite(bus.clock.pin, 0);
    pinMode(bus.rw.pin, OUTPUT);
    digitalWrite(bus.rw.pin, 1);

    pinMode(bus.extDecode.pin, OUTPUT);
    digitalWrite(bus.extDecode.pin, 0);

    for (int bit = 0; bit < 16; ++bit) {
        pinMode(bus.addr.pin + bit,
            ((fakeBusTestAddress >> bit) & 1) == 1 ? INPUT_PULLUP : INPUT_PULLDOWN);
    }
    pinMode(bus.addr.pin, OUTPUT);
    digitalWrite(bus.addr.pin, 0);
    fakeBusSignalsStart();

    pinMode(bus.mpd.pin, INPUT_PULLDOWN);
#ifdef HAVE_REFRESH
    pinMode(bus.refresh_.pin, INPUT_PULLUP);
#endif
    pinMode(bus.extSel.pin, INPUT_PULLUP);
}

// The test page's read data: the two A0 addresses read 0xff and 0x00, so
// D0/D1 change between them.
static DRAM_ATTR uint8_t fakeTestPageData[256];

void setupFakeBusMmu() {
    const uint16_t fakeTestAddress = fakeBusTestAddress;
    const int fakeTestPage = pageNr(fakeTestAddress);
    L1Block &block = l1Blocks[page2L1Block(fakeTestPage)];
    const int rd = (fakeTestPage & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_RD;
    const int wr = (fakeTestPage & pageInL1BlockMask) | PAGESEL_CPU | PAGESEL_WR;
    // Map the page as board memory whatever baseMemSz is: reads from
    // fakeTestPageData with extSel_ and the data bus driven; writes to dummy
    // RAM with extSel_. (Base RAM writes normally leave extSel_ alone, ctrl 0,
    // and fall through to the Atari's RAM; claiming them puts the write
    // path's extSel_ drive and release in the profile too.) With no extended
    // memory, extBanks[] points at this same base L1Block for $4000, so a
    // test address there exercises core 1's $4000-block select.
    block.pages[rd] = fakeTestPageData;
    block.ctrl[rd] = bus.data.mask | bus.extSel.mask;
    block.pages[wr] = dummyRam;
    block.ctrl[wr] = bus.extSel.mask;
    fakeTestPageData[fakeTestAddress & 0xff] = 0xff;
    fakeTestPageData[(fakeTestAddress + 1) & 0xff] = 0x00;
    printf("Fake bus test page %02x: reads from a test buffer (%04x=ff, %04x=00), writes to dummy RAM, both with extSel_\n",
        fakeTestPage, fakeTestAddress, fakeTestAddress + 1);
}
#endif  // FAKE_CLOCK
