#include "i2s_private.h"
#include "gdma_priv.h"
#include "esp_cache.h"
#include <stdlib.h>

esp_err_t i2sDmaPocSetDirectTxBuffer(i2s_chan_handle_t channel, uint8_t *buffer, size_t buffer_size)
{
    if (!channel || !buffer || !buffer_size || ((uintptr_t)buffer & 0x0F)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((buffer_size & 0x03) || (channel->dma.buf_size & 0x03)) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t offset = 0;
    for (uint32_t i = 0; i < channel->dma.desc_num; ++i) {
        if (offset >= buffer_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        size_t chunk_size = buffer_size - offset;
        if (chunk_size > channel->dma.buf_size) {
            chunk_size = channel->dma.buf_size;
        }

        lldesc_t *desc = channel->dma.desc[i];
        desc->buf = buffer + offset;
        desc->length = chunk_size;
        desc->size = chunk_size;
        desc->owner = 1;
        desc->eof = 1;
        desc->sosf = 0;
        desc->offset = 0;
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
        esp_cache_msync(desc,
                        sizeof(lldesc_t),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#endif
        offset += chunk_size;
    }

    if (offset != buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint32_t i = 0; i < channel->dma.desc_num; ++i) {
        free(channel->dma.bufs[i]);
        channel->dma.bufs[i] = NULL;
    }
    free(channel->dma.bufs);
    channel->dma.bufs = NULL;

    return ESP_OK;
}

void i2sDmaPocDisableGdmaTxInterrupts(i2s_chan_handle_t channel)
{
    gdma_channel_handle_t dma_channel = channel->dma.dma_chan;
    gdma_pair_t *pair = dma_channel->pair;

    portENTER_CRITICAL(&pair->spinlock);
    gdma_hal_enable_intr(&pair->group->hal,
                         pair->pair_id,
                         GDMA_CHANNEL_DIRECTION_TX,
                         GDMA_LL_EVENT_TX_EOF,
                         false);
    gdma_hal_clear_intr(&pair->group->hal,
                        pair->pair_id,
                        GDMA_CHANNEL_DIRECTION_TX,
                        GDMA_LL_EVENT_TX_EOF);
    portEXIT_CRITICAL(&pair->spinlock);
}
