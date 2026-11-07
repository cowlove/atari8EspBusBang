#pragma once

#include "esp_err.h"
#include "driver/i2s_std.h"
#include "soc/lldesc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buffer;
    size_t size;
} i2s_dma_segment_t;

/* Both attach functions can move the channel's DMA descriptors into
 * caller memory (descriptors: one lldesc_t per channel descriptor, 4-byte
 * aligned, DMA-capable internal RAM), so they sit where the caller chose,
 * for example beside the buffers, instead of scattered over the heap; NULL
 * keeps the driver's. Caller descriptors must be handed back with
 * i2s_dma_ring_release_descriptors() before the channel is deleted (the
 * driver would free them), and the channel must not be reconfigured after
 * attaching (the driver would reallocate them). */

/* Self-tending TX ring: point the channel's descriptors (one per segment,
 * linked in a loop) at the caller's segments and mask TX EOF interrupts.
 * Segments are 4-byte aligned, a multiple of 4 bytes and no larger than the
 * channel's DMA buffer size. */
esp_err_t i2s_dma_tx_ring_attach(i2s_chan_handle_t channel,
                                 const i2s_dma_segment_t *segments,
                                 size_t segment_count,
                                 lldesc_t *descriptors);

/* Enable a TX channel prepared by i2s_dma_tx_ring_attach() and keep its EOF
 * interrupt masked afterwards. */
esp_err_t i2s_dma_tx_ring_enable(i2s_chan_handle_t channel);

/* Autonomous RX ring: split buffer into the channel's descriptors (which must
 * come out equal), program rx_eof_num to match and mask RX interrupts. */
esp_err_t i2s_dma_rx_ring_attach(i2s_chan_handle_t channel,
                                 uint8_t *buffer,
                                 size_t buffer_size,
                                 lldesc_t *descriptors);

/* Before deleting a channel: drop its references to the caller descriptors
 * given to an attach function (any it doesn't hold are left alone). */
void i2s_dma_ring_release_descriptors(i2s_chan_handle_t channel, const lldesc_t *descriptors);

/* Enable an RX channel prepared by i2s_dma_rx_ring_attach() and keep its
 * EOF interrupt masked afterwards. */
esp_err_t i2s_dma_rx_ring_enable(i2s_chan_handle_t channel);

/* The address span [*first, *end) of the channel's DMA descriptors, for
 * checking where in SRAM they landed. */
esp_err_t i2s_dma_desc_span(i2s_chan_handle_t channel, uintptr_t *first, uintptr_t *end);
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
