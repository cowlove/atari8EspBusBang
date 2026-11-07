#include "i2s_dma_ring.h"

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#if SOC_GDMA_SUPPORTED
#include "gdma_priv.h"
#include "hal/gdma_ll.h"
#endif
#include "hal/i2s_ll.h"
#include "i2s_private.h"

/* IDF 5.5's gdma_priv.h defines TAG as a macro. */
#undef TAG

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "i2s_dma_ring";

/* Both rings replace the driver's DMA buffers with the caller's memory, so
 * the driver-allocated buffers are freed. */
static void free_driver_buffers(i2s_chan_handle_t channel)
{
    for (uint32_t index = 0U; index < channel->dma.desc_num; ++index) {
        free(channel->dma.bufs[index]);
        channel->dma.bufs[index] = NULL;
    }
    free(channel->dma.bufs);
    channel->dma.bufs = NULL;
}

/* Move the channel's descriptors into the caller's array, freeing the
 * driver's, and link them in a loop as the driver did. */
static void adopt_descriptors(i2s_chan_handle_t channel, lldesc_t *descriptors)
{
    if (!descriptors) {
        return;
    }
    const uint32_t count = channel->dma.desc_num;
    for (uint32_t index = 0U; index < count; ++index) {
        if (channel->dma.desc[index] != &descriptors[index]) {
            free(channel->dma.desc[index]);
            channel->dma.desc[index] = &descriptors[index];
        }
    }
    for (uint32_t index = 0U; index < count; ++index) {
        memset(&descriptors[index], 0, sizeof(descriptors[index]));
        STAILQ_NEXT(&descriptors[index], qe) = &descriptors[(index + 1U) % count];
    }
}

void i2s_dma_ring_release_descriptors(i2s_chan_handle_t channel, const lldesc_t *descriptors)
{
    if (!channel || !descriptors || !channel->dma.desc) {
        return;
    }
    for (uint32_t index = 0U; index < channel->dma.desc_num; ++index) {
        if (channel->dma.desc[index] == &descriptors[index]) {
            channel->dma.desc[index] = NULL;   /* the driver skips NULL */
        }
    }
}

static void point_descriptor(lldesc_t *descriptor, uint8_t *buffer, size_t size)
{
    descriptor->buf = buffer;
    descriptor->length = size;
    descriptor->size = size;
    descriptor->owner = 1U;
    descriptor->eof = 1U;
    descriptor->sosf = 0U;
    descriptor->offset = 0U;
}

esp_err_t i2s_dma_tx_ring_attach(i2s_chan_handle_t channel,
                                 const i2s_dma_segment_t *segments,
                                 size_t segment_count,
                                 lldesc_t *descriptors)
{
    if (!channel || !segments || channel->dir != I2S_DIR_TX ||
        channel->dma.desc_num != segment_count || ((uintptr_t)descriptors & 0x03U)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0; index < segment_count; ++index) {
        if (!segments[index].buffer || !segments[index].size ||
            (segments[index].size % (2U * sizeof(uint16_t))) != 0U ||
            ((uintptr_t)segments[index].buffer & 0x03U) ||
            segments[index].size > channel->dma.buf_size) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    adopt_descriptors(channel, descriptors);
    for (size_t index = 0; index < segment_count; ++index) {
        point_descriptor(channel->dma.desc[index], segments[index].buffer,
                         segments[index].size);
    }
    free_driver_buffers(channel);

#if SOC_GDMA_SUPPORTED
    gdma_pair_t *pair = channel->dma.dma_chan->pair;
    portENTER_CRITICAL(&pair->spinlock);
    gdma_hal_enable_intr(&pair->group->hal, pair->pair_id, GDMA_CHANNEL_DIRECTION_TX,
                         GDMA_LL_EVENT_TX_EOF, false);
    gdma_hal_clear_intr(&pair->group->hal, pair->pair_id, GDMA_CHANNEL_DIRECTION_TX,
                        GDMA_LL_EVENT_TX_EOF);
    portEXIT_CRITICAL(&pair->spinlock);
#endif
    /* Without GDMA (classic ESP32) the TX EOF interrupt lives in the I2S
     * peripheral and i2s_channel_enable() re-enables it, so it is masked in
     * i2s_dma_tx_ring_enable() instead. */
    ESP_LOGI(TAG, "TX ring: %u descriptors", (unsigned)segment_count);
    return ESP_OK;
}

esp_err_t i2s_dma_tx_ring_enable(i2s_chan_handle_t channel)
{
    esp_err_t err = i2s_channel_enable(channel);
    if (err != ESP_OK) {
        return err;
    }
#if !SOC_GDMA_SUPPORTED
    i2s_dev_t *dev = channel->controller->hal.dev;
    i2s_ll_enable_intr(dev, I2S_LL_TX_EVENT_MASK | I2S_LL_EVENT_TX_DSCR_ERR, false);
    i2s_ll_clear_intr_status(dev, I2S_LL_TX_EVENT_MASK | I2S_LL_EVENT_TX_DSCR_ERR);
#endif
    return ESP_OK;
}

esp_err_t i2s_dma_rx_ring_attach(i2s_chan_handle_t channel,
                                 uint8_t *buffer,
                                 size_t buffer_size,
                                 lldesc_t *descriptors)
{
    if (!channel || !buffer || buffer_size == 0U || (buffer_size & 3U) != 0U ||
        ((uintptr_t)buffer & 0x0fU) != 0U || channel->dir != I2S_DIR_RX ||
        channel->dma.desc_num < 2U || ((uintptr_t)descriptors & 0x03U)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The I2S RX peripheral ends the current DMA descriptor every
     * rx_eof_num bytes. Unless every descriptor is exactly that size, a
     * descriptor gets closed early, its tail is never written, and the ring
     * period no longer matches the wire cycle. Require equal descriptors and
     * program rx_eof_num to match. */
    size_t descriptor_count = channel->dma.desc_num;
    size_t descriptor_size = buffer_size / descriptor_count;
    if (descriptor_size * descriptor_count != buffer_size || (descriptor_size & 3U) != 0U ||
        descriptor_size > channel->dma.buf_size || descriptor_size > 4095U) {
        return ESP_ERR_INVALID_SIZE;
    }

    adopt_descriptors(channel, descriptors);
    for (size_t index = 0U; index < descriptor_count; ++index) {
        point_descriptor(channel->dma.desc[index], buffer + index * descriptor_size,
                         descriptor_size);
    }
    free_driver_buffers(channel);

#if SOC_GDMA_SUPPORTED
    gdma_pair_t *pair = channel->dma.dma_chan->pair;
    portENTER_CRITICAL(&pair->spinlock);
    gdma_hal_enable_intr(&pair->group->hal, pair->pair_id, GDMA_CHANNEL_DIRECTION_RX,
                         UINT32_MAX, false);
    gdma_hal_clear_intr(&pair->group->hal, pair->pair_id, GDMA_CHANNEL_DIRECTION_RX,
                        UINT32_MAX);
    portEXIT_CRITICAL(&pair->spinlock);
#endif
    /* Without GDMA (classic ESP32) the RX EOF interrupt lives in the I2S
     * peripheral and i2s_channel_enable() re-enables it, so it is masked in
     * i2s_dma_rx_ring_enable() instead. i2s_ll_rx_set_eof_num() takes bytes on
     * both chips. */
    i2s_ll_rx_set_eof_num(channel->controller->hal.dev, (int)descriptor_size);

    ESP_LOGI(TAG, "RX ring: %u bytes as %u x %u-byte descriptors, rx_eof_num=%u",
             (unsigned)buffer_size, (unsigned)descriptor_count,
             (unsigned)descriptor_size, (unsigned)descriptor_size);
    return ESP_OK;
}

esp_err_t i2s_dma_rx_ring_enable(i2s_chan_handle_t channel)
{
    esp_err_t err = i2s_channel_enable(channel);
    if (err != ESP_OK) {
        return err;
    }
#if !SOC_GDMA_SUPPORTED
    i2s_dev_t *dev = channel->controller->hal.dev;
    i2s_ll_enable_intr(dev, I2S_LL_RX_EVENT_MASK | I2S_LL_EVENT_RX_DSCR_ERR, false);
    i2s_ll_clear_intr_status(dev, I2S_LL_RX_EVENT_MASK | I2S_LL_EVENT_RX_DSCR_ERR);
#endif
    return ESP_OK;
}

esp_err_t i2s_dma_rx_ring_restart(i2s_chan_handle_t channel)
{
    esp_err_t err = i2s_channel_disable(channel);
    if (err != ESP_OK) {
        return err;
    }
    return i2s_dma_rx_ring_enable(channel);
}

esp_err_t i2s_dma_desc_span(i2s_chan_handle_t channel, uintptr_t *first, uintptr_t *end)
{
    if (!channel || !first || !end || channel->dma.desc_num == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    uintptr_t low = UINTPTR_MAX, high = 0U;
    for (uint32_t index = 0U; index < channel->dma.desc_num; ++index) {
        const uintptr_t address = (uintptr_t)channel->dma.desc[index];
        low = address < low ? address : low;
        high = address + sizeof(lldesc_t) > high ? address + sizeof(lldesc_t) : high;
    }
    *first = low;
    *end = high;
    return ESP_OK;
}

esp_err_t i2s_dma_rx_progress_init(i2s_chan_handle_t channel, i2s_dma_rx_progress_t *progress)
{
    if (!channel || !progress || channel->dir != I2S_DIR_RX ||
        channel->dma.desc_num > I2S_DMA_RX_PROGRESS_MAX_DESC) {
        return ESP_ERR_INVALID_ARG;
    }
    progress->count = channel->dma.desc_num;
    for (size_t index = 0; index < progress->count; ++index) {
        lldesc_t *descriptor = channel->dma.desc[index];
        descriptor->length = 0U;
        progress->desc[index] = descriptor;
    }
    return ESP_OK;
}

/* Clearing length races with the DMA's own write-back of the same word, but
 * only informational fields can be lost that way: RX uses size, not length,
 * and owner checking is off. The worst case is one missed completion. */
uint32_t IRAM_ATTR i2s_dma_rx_progress_poll(i2s_dma_rx_progress_t *progress)
{
    uint32_t completed = 0U;
    for (size_t index = 0; index < progress->count; ++index) {
        volatile lldesc_t *descriptor = progress->desc[index];
        if (descriptor->length != 0U) {
            descriptor->length = 0U;
            ++completed;
        }
    }
    return completed;
}

void i2s_dma_rx_debug_dump(i2s_chan_handle_t channel)
{
#if !SOC_GDMA_SUPPORTED
    i2s_dev_t *dev = channel->controller->hal.dev;
    ESP_LOGW(TAG, "RXREG conf=%08" PRIx32 " int_raw=%08" PRIx32 " fifo_conf=%08" PRIx32
             " conf_chan=%08" PRIx32 " rx_eof_num=%" PRIu32 " conf1=%08" PRIx32
             " conf2=%08" PRIx32 " clkm=%08" PRIx32 " srate=%08" PRIx32 " state=%08" PRIx32,
             dev->conf.val, dev->int_raw.val, dev->fifo_conf.val, dev->conf_chan.val,
             dev->rx_eof_num, dev->conf1.val, dev->conf2.val, dev->clkm_conf.val,
             dev->sample_rate_conf.val, dev->state.val);
    ESP_LOGW(TAG, "RXDMA in_link=%08" PRIx32 " in_link_dscr=%08" PRIx32 " bf0=%08" PRIx32
             " bf1=%08" PRIx32 " in_eof_des=%08" PRIx32 " lc_conf=%08" PRIx32
             " lc_state0=%08" PRIx32 " lc_state1=%08" PRIx32 " ahb_test=%08" PRIx32,
             dev->in_link.val, dev->in_link_dscr, dev->in_link_dscr_bf0, dev->in_link_dscr_bf1,
             dev->in_eof_des_addr, dev->lc_conf.val, dev->lc_state0, dev->lc_state1,
             dev->ahb_test.val);
    for (uint32_t index = 0U; index < channel->dma.desc_num; ++index) {
        lldesc_t *d = channel->dma.desc[index];
        ESP_LOGW(TAG, "RXDESC %" PRIu32 " @%p size=%u length=%u owner=%u eof=%u buf=%p next=%p",
                 index, d, (unsigned)d->size, (unsigned)d->length, (unsigned)d->owner,
                 (unsigned)d->eof, d->buf, (void *)d->empty);
    }
#else
    (void)channel;
#endif
}

void i2s_dma_rx_snapshot(i2s_chan_handle_t channel, i2s_dma_rx_snapshot_t *snapshot)
{
    *snapshot = (i2s_dma_rx_snapshot_t){0};
    snapshot->ccount = esp_cpu_get_cycle_count();
#if !SOC_GDMA_SUPPORTED
    i2s_dev_t *dev = channel->controller->hal.dev;
    snapshot->in_link = dev->in_link.val;
    snapshot->in_link_dscr = dev->in_link_dscr;
    snapshot->dscr_bf0 = dev->in_link_dscr_bf0;
    snapshot->dscr_bf1 = dev->in_link_dscr_bf1;
    snapshot->lc_state0 = dev->lc_state0;
    snapshot->lc_state1 = dev->lc_state1;
    snapshot->int_raw = dev->int_raw.val;
    snapshot->in_eof_des = dev->in_eof_des_addr;
    snapshot->conf = dev->conf.val;
    if (channel->dma.desc && channel->dma.desc[0]) {
        snapshot->desc0_word0 = *(volatile uint32_t *)channel->dma.desc[0];
    }
#else
    (void)channel;
#endif
}
