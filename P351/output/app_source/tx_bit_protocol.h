#ifndef TX_BIT_PROTOCOL_H
#define TX_BIT_PROTOCOL_H

#include "xil_types.h"
#include "pn11_frame.h"
#include "board_config.h"

/* Nonzero and unmatched by every valid profile: block all STARTs after a
 * failed hardware configuration until a complete valid transaction arrives. */
#define TXB1_GUARD_BLOCKED 255U

#define TXB1_HEADER_SIZE       40U
#define TXB1_MAX_DATA_PAYLOAD  1024U
#define TXB1_MAX_FRAME_BYTES   (1024U * 1024U)

void tx_bit_protocol_init(void);
u8 tx_bit_protocol_handle(const u8 *datagram, u16 length);
u32 tx_bit_protocol_ack_result(void);
/* BQRY: bit0 begun, bit1 committed, bit2 PL progress/rate/length verified, bit3 pending,
 * bit4 active, bit5 hardware/start error, bit6 matching task. */
u32 tx_bit_protocol_query(u32 task_id);
void tx_bit_protocol_service(void);
/* Abort any queued/in-progress download when the waveform mode changes. */
void tx_bit_protocol_abort(void);
/* Quiesce a waveform change without discarding a fully committed task that
 * has not been started yet.  This supports the deterministic UI transaction:
 * download/COMMIT -> waveform configuration -> START. */
void tx_bit_protocol_mode_change(void);
/* Require TXB1 START to match the active standard modulator.  profile=0
 * disables the guard for the RAW BIT diagnostic path. */
void tx_bit_protocol_set_standard_guard(u8 profile, u32 bit_rate_word);

/* Foreground-only local PN11 task: prepare -> configure TX1/modulator ->
 * start. No UDP download is needed. profile=2 PCM/FM or 4 SOQPSK; TX1 only.
 * Start queues the existing foreground service and is not RF/DMA proof.
 * Local tasks use the shared buffer and query/status path; prepare fails
 * while DMA, a pending command, or an incomplete network download owns it. */
enum { TX_LOCAL_OK=0, TX_LOCAL_BAD_CONFIG=1, TX_LOCAL_BUSY=2,
       TX_LOCAL_BUFFER_LIMIT=3, TX_LOCAL_MODULATOR_MISMATCH=4 };
u32 tx_bit_protocol_prepare_local_pn11(const pn11_frame_config_t *cfg,
                                     u32 task_id, u32 bit_rate,
                                     u32 repeat_count, u8 profile);
u32 tx_bit_protocol_start_local_pn11(u32 task_id);
/* 主循环只读快照：不启动/停止任务，不读取或改写数据缓存。 */
void tx_bit_protocol_snapshot(u32 words[BOARD_CONFIG_WORDS]);

#endif
