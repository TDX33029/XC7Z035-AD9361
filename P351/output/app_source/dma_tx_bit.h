#ifndef DMA_TX_BIT_H
#define DMA_TX_BIT_H

#include "xil_types.h"
#include "xstatus.h"

/* error_code低位保留PL错误；高位表示PS检测到的DMA/握手故障。STOP保留，CLEAR清除。 */
#define DMA_TX_BIT_ERR_DMA        0x80000000U /* OR入MM2S DMASR错误位 */
#define DMA_TX_BIT_ERR_RESET      0x40000001U
#define DMA_TX_BIT_ERR_ARM        0x40000002U
#define DMA_TX_BIT_ERR_COMPLETION 0x40000003U
#define DMA_TX_BIT_ERR_ENGINE     0x20000000U /* OR入PL错误码，避免STOP清状态后丢失 */

typedef struct {
    u32 engine_status;
    u32 bits_sent;
    u32 error_code;
    u32 dma_busy;
    u32 active;
    u32 completed_frames;
    u32 configured_bits;
    u32 configured_bit_rate;
} dma_tx_bit_status_t;

int dma_tx_bit_init(void);
int dma_tx_bit_start(const u8 *data, u32 valid_bits, u32 repeat_count,
                     u32 profile, u32 channel, u32 bit_rate);
void dma_tx_bit_stop(void);
void dma_tx_bit_clear(void);
/* Put the DMA/bit engine in a known idle state before mode/RF changes. */
int dma_tx_bit_quiesce(void);
void dma_tx_bit_service(void);
void dma_tx_bit_get_status(dma_tx_bit_status_t *status);
u8 dma_tx_bit_is_active(void);

#endif
