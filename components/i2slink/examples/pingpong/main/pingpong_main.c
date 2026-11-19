/* i2slink ping-pong: round-trip latency and echo throughput between two
 * ESP32-S3s (or one, in loopback).
 *
 * PING (the clock source) sends a ping and waits for its echo, over and
 * over, in two phases:
 *   - latency: short pings (16 bytes), each sent at a random moment, as
 *     real requests are; prints min/mean/max round trip and a histogram;
 *   - bulk: pings of PINGPONG_BULK_BYTES, the echo checked byte for byte;
 *     prints the echo rate (bytes each way per second).
 * PONG (the clock follower) echoes each ping back with its tag changed.
 * LOOPBACK (one board, no wires) runs PING's loop against its own TX: each
 * ping comes back as itself, which times the link's one-way pipeline.
 *
 * The link task runs on core 1 and busy-polls (the link's calls never
 * block on an RTOS object), so nothing else should need core 1. */

#include "i2slink.h"

#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define LATENCY_PINGS 2000U
#define LATENCY_BYTES 16U
#define BULK_SECONDS 3U
#define REPLY_TIMEOUT_US 20000U   /* a lost ping or echo: give up and send the next */
#define HIST_BUCKET_US 100U
#define HIST_BUCKETS 12U

#if CONFIG_PINGPONG_BULK_BYTES > I2SLINK_PAYLOAD_BYTES
#error "PINGPONG_BULK_BYTES is more than the link's payload (I2SLINK_PAYLOAD_BYTES; see I2SLINK_LINE_COUNT)"
#endif

/* A ping: tag, number, then a pattern the echo must carry back unchanged. */
typedef struct {
    char tag[4];
    uint32_t n;
} ping_header_t;

static i2slink_t *link;
static uint8_t rx[I2SLINK_PAYLOAD_BYTES];

#if CONFIG_PINGPONG_ROLE_LOOPBACK
static const char kEchoTag[4] = {'P', 'I', 'N', 'G'};   /* our own ping comes back */
#else
static const char kEchoTag[4] = {'P', 'O', 'N', 'G'};
#endif

static void print_link_stats(const char *who)
{
    i2slink_stats_t s;
    i2slink_stats(link, &s);
    printf("%s link: writes %" PRIu32 ", locks %" PRIu32 ", torn %" PRIu32 ", CRC errors %" PRIu32
           ", skipped %" PRIu32 ", slot tunes %" PRIu32 "\n",
           who, s.writes, s.locks, s.torn, s.crc_errors, s.skipped, s.slot_tunes);
}

#if CONFIG_PINGPONG_ROLE_PONG

static void link_task(void *arg)
{
    (void)arg;
    uint32_t echoed = 0, faults = 0;
    int64_t next_report = esp_timer_get_time() + 5000000;
    for (;;) {
        uint32_t sequence;
        const i2slink_status_t st = i2slink_read(link, rx, &sequence, 100000);
        if (st == I2SLINK_NEW) {
            const ping_header_t *ping = (const ping_header_t *)rx;
            if (memcmp(ping->tag, "PING", 4) == 0) {
                memcpy(rx, kEchoTag, 4);
                i2slink_write(link, rx, i2slink_last_length(link));
                ++echoed;
            }
        } else if (st != I2SLINK_TIMEOUT) {
            ++faults;   /* no lock yet (PING not running), or a link fault */
        }
        if (esp_timer_get_time() >= next_report) {
            next_report += 5000000;
            printf("PONG: %" PRIu32 " pings echoed, %" PRIu32 " link faults\n", echoed, faults);
            print_link_stats("PONG");
        }
    }
}

#else  /* PING, LOOPBACK */

static uint8_t tx[I2SLINK_PAYLOAD_BYTES];

static uint8_t pattern(uint32_t n, size_t i)
{
    return (uint8_t)(n * 31U + i * 7U);
}

/* Send ping n of length bytes and wait for its echo. Returns the round trip
 * in microseconds, or -1 if none came (lost, or the link faulted). On a
 * wrong echo payload, counts it in *mismatches. */
static int32_t ping_once(uint32_t n, size_t length, uint32_t *mismatches)
{
    ping_header_t *ping = (ping_header_t *)tx;
    memcpy(ping->tag, "PING", 4);
    ping->n = n;
    for (size_t i = sizeof(*ping); i < length; ++i) {
        tx[i] = pattern(n, i);
    }
    const int64_t start = esp_timer_get_time();
    i2slink_write(link, tx, length);
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (now - start > REPLY_TIMEOUT_US) {
            return -1;
        }
        uint32_t sequence;
        if (i2slink_read(link, rx, &sequence, REPLY_TIMEOUT_US) != I2SLINK_NEW) {
            continue;
        }
        const ping_header_t *echo = (const ping_header_t *)rx;
        if (memcmp(echo->tag, kEchoTag, 4) != 0 || echo->n != n) {
            continue;   /* an older echo */
        }
        const int32_t rtt = (int32_t)(esp_timer_get_time() - start);
        if (i2slink_last_length(link) != length || memcmp(rx + sizeof(*ping), tx + sizeof(*ping),
                                                           length - sizeof(*ping)) != 0) {
            ++*mismatches;
        }
        return rtt;
    }
}

static void link_task(void *arg)
{
    (void)arg;
    uint32_t n = esp_random();
    uint32_t mismatches = 0;

    printf("PING: waiting for the link (is PONG running and wired?)\n");
    while (ping_once(n++, LATENCY_BYTES, &mismatches) < 0) {
    }
    printf("PING: link up\n");

    for (unsigned round = 1;; ++round) {
        /* Latency: short pings, each at a random moment, so the wait for
         * this end's TX slot (0..1 wire cycle) is sampled evenly. */
        uint32_t hist[HIST_BUCKETS] = {0};
        uint32_t ok = 0, lost = 0;
        int32_t lo = INT32_MAX, hi = 0;
        int64_t sum = 0;
        for (uint32_t i = 0; i < LATENCY_PINGS; ++i) {
            esp_rom_delay_us(esp_random() % 1000U);
            const int32_t rtt = ping_once(n++, LATENCY_BYTES, &mismatches);
            if (rtt < 0) {
                ++lost;
                continue;
            }
            ++ok;
            sum += rtt;
            lo = rtt < lo ? rtt : lo;
            hi = rtt > hi ? rtt : hi;
            const uint32_t b = (uint32_t)rtt / HIST_BUCKET_US;
            ++hist[b < HIST_BUCKETS ? b : HIST_BUCKETS - 1U];
        }
        printf("\nround %u, latency (%u-byte pings at random times): %" PRIu32 " ok, %" PRIu32
               " lost; round trip us min %" PRId32 " mean %" PRId32 " max %" PRId32 "\n",
               round, LATENCY_BYTES, ok, lost, ok ? lo : 0, ok ? (int32_t)(sum / ok) : 0, hi);
        for (uint32_t b = 0; b < HIST_BUCKETS; ++b) {
            if (hist[b]) {
                if (b == HIST_BUCKETS - 1U) {
                    printf("  %4" PRIu32 "+     us: %5" PRIu32 "\n", b * HIST_BUCKET_US, hist[b]);
                } else {
                    printf("  %4" PRIu32 "-%4" PRIu32 " us: %5" PRIu32 "\n", b * HIST_BUCKET_US,
                           (b + 1U) * HIST_BUCKET_US - 1U, hist[b]);
                }
            }
        }

        /* Bulk: back-to-back pings of BULK_BYTES, each echo checked. */
        const size_t length = CONFIG_PINGPONG_BULK_BYTES;
        uint32_t bulk_ok = 0, bulk_lost = 0;
        const int64_t start = esp_timer_get_time();
        while (esp_timer_get_time() - start < BULK_SECONDS * 1000000LL) {
            if (ping_once(n++, length, &mismatches) < 0) {
                ++bulk_lost;
            } else {
                ++bulk_ok;
            }
        }
        const int64_t elapsed = esp_timer_get_time() - start;
        printf("round %u, bulk (%u-byte pings): %" PRIu32 " echoed, %" PRIu32 " lost, %.1f kB/s each way; "
               "echo mismatches so far %" PRIu32 "\n",
               round, (unsigned)length, bulk_ok, bulk_lost, (double)bulk_ok * length * 1000.0 / elapsed,
               mismatches);
        print_link_stats("PING");
    }
}

#endif

void app_main(void)
{
    const i2slink_config_t config = {
#if CONFIG_PINGPONG_ROLE_PONG
        .clock = I2SLINK_CLOCK_FOLLOWER,
#else
        .clock = I2SLINK_CLOCK_SOURCE,
#endif
        .i2s_port = 0,
        .bclk_gpio = CONFIG_PINGPONG_BCLK_GPIO,
        .tx_gpio = CONFIG_PINGPONG_TX_GPIO,
#if CONFIG_PINGPONG_ROLE_LOOPBACK
        .rx_gpio = CONFIG_PINGPONG_TX_GPIO,   /* RX listens on our own TX pin */
        .loopback = true,
#else
        .rx_gpio = CONFIG_PINGPONG_RX_GPIO,
        .loopback = false,
#endif
        .sync_timeout_us = 0,   /* default: four wire cycles */
        /* One end (here the follower) shifts its TX until the other end
         * reads it aligned, and moves its frame to just after where its
         * fast replies land (slot tuning): the shortest round trips. */
#if CONFIG_PINGPONG_ROLE_PONG
        .tx_auto_sync = true,
#else
        .tx_auto_sync = false,
#endif
        .log = NULL,   /* printf */
    };
    printf("i2slink ping-pong: %s, BCLK GPIO%d, TX GPIO%d, RX GPIO%d; %u frames/s, %u-byte payload, "
           "%u-byte wire cycle (%u us)\n",
#if CONFIG_PINGPONG_ROLE_PING
           "PING",
#elif CONFIG_PINGPONG_ROLE_PONG
           "PONG",
#else
           "LOOPBACK",
#endif
           config.bclk_gpio, config.tx_gpio, config.rx_gpio, (unsigned)I2SLINK_FRAMES_PER_SECOND,
           (unsigned)I2SLINK_PAYLOAD_BYTES, (unsigned)I2SLINK_CYCLE_BYTES, (unsigned)I2SLINK_CYCLE_US);
    ESP_ERROR_CHECK(i2slink_init(&link, &config));
    xTaskCreatePinnedToCore(link_task, "i2slink", 4096, NULL, 5, NULL, 1);
}
