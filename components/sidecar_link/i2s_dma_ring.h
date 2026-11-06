#pragma once

#include "esp_err.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buffer;
    size_t size;
} i2s_dma_segment_t;

/* Self-tending TX ring: point the channel's descriptors (one per segment,
 * linked in a loop by the driver) at the caller's segments and mask TX EOF
 * interrupts. Segments are 4-byte aligned, a multiple of 4 bytes and no
 * larger than the channel's DMA buffer size. */
esp_err_t i2s_dma_tx_ring_attach(i2s_chan_handle_t channel,
                                 const i2s_dma_segment_t *segments,
                                 size_t segment_count);

/* Enable a TX channel prepared by i2s_dma_tx_ring_attach() and keep its EOF
 * interrupt masked afterwards. */
esp_err_t i2s_dma_tx_ring_enable(i2s_chan_handle_t channel);

/* Autonomous RX ring: split buffer into the channel's descriptors (which must
 * come out equal), program rx_eof_num to match and mask RX interrupts. */
esp_err_t i2s_dma_rx_ring_attach(i2s_chan_handle_t channel,
                                 uint8_t *buffer,
                                 size_t buffer_size);

/* Enable an RX channel prepared by i2s_dma_rx_ring_attach() and keep its
 * EOF interrupt masked afterwards. */
esp_err_t i2s_dma_rx_ring_enable(i2s_chan_handle_t channel);
/* Setup-level recovery (driver calls; not for the real-time path): disable
 * the RX channel and enable it again. The driver resets the RX core, DMA and
 * FIFO and restarts the link at descriptor 0; the ring stays attached. */
esp_err_t i2s_dma_rx_ring_restart(i2s_chan_handle_t channel);


/* RX DMA progress, by memory only. The DMA writes each descriptor's length
 * back when it completes it; the poll clears every nonzero length and counts
 * them, so a nonzero count means data is still flowing. Setup reads driver
 * structures; the poll itself touches only descriptor memory (no peripheral
 * registers), so it is safe on a core that must not disturb the bus. */
#define I2S_DMA_RX_PROGRESS_MAX_DESC 16U
typedef struct {
    void *desc[I2S_DMA_RX_PROGRESS_MAX_DESC];
    size_t count;
} i2s_dma_rx_progress_t;

esp_err_t i2s_dma_rx_progress_init(i2s_chan_handle_t channel, i2s_dma_rx_progress_t *progress);
/* Number of descriptors completed since the last poll. */
uint32_t i2s_dma_rx_progress_poll(i2s_dma_rx_progress_t *progress);

/* Diagnostic only (registers, logging; not for the real-time path): log an
 * RX channel's I2S/DMA registers and descriptors. Classic ESP32 only. */
void i2s_dma_rx_debug_dump(i2s_chan_handle_t channel);

/* Diagnostic only: raw I2S RX/DMA state at one instant, cheap enough to take
 * between init steps without disturbing their timing (register loads, no
 * logging). Classic ESP32 only; zeros elsewhere. */
typedef struct {
    uint32_t ccount;
    uint32_t in_link, in_link_dscr, dscr_bf0, dscr_bf1;
    uint32_t lc_state0, lc_state1, int_raw, in_eof_des, conf;
    uint32_t desc0_word0;   /* first descriptor's size/length/owner word */
} i2s_dma_rx_snapshot_t;

void i2s_dma_rx_snapshot(i2s_chan_handle_t channel, i2s_dma_rx_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
