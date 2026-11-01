#ifndef P351_RX_RECEIVER_H
#define P351_RX_RECEIVER_H
#include "xil_types.h"
/* RX2独立控制；不控制TX功率、不代替电气/射频验收。 */
typedef struct {
    u64 center_hz;
    u32 bit_rate,bandwidth_hz;
    s32 gain_db;
    u64 sync_word;
    u16 frame_bytes,frame_count;
    u8 enabled,mode,line_code; /* mode 0=PCM/FM,1=SOQPSK；PCM line 0..6 */
    u8 sync_bits,sfid_start; /* 16..33位同步；ID型复帧首编号0或1 */
} rx_receiver_config_t;
#define RXR_CFG_WORDS 31U
#define RXR_CTRL 0x20U
#define RXR_RATE 0x24U
#define RXR_SYNC 0x28U
#define RXR_FRAME 0x2CU
#define RXR_STATUS 0x30U
#define RXR_DATA 0x34U
#define RXR_SELECT 0x38U
#define RXR_COUNTER 0x3CU
#define RXR_ABI 0x52395831U
/* RXC2 48B -> RXAK nonce/result；保留RXC1的32位同步/单帧输入。
 * RXQ1 8B -> RXA1 version3+31u32LE。
 * 返回0=数字配置成功，1=非法参数，2=不匹配PL，3=BUSY/未就绪，
 * 4=RF回读失败，5=PL写入失败，6=停止超时，7=缺样或帧同步完整性故障，8=位流FIFO溢出。
 * 9=码钟估计快照超时。w24=有符号Q24码钟微调，w25=瞬时NCO码率字。
 * w26=同步字高位，w27=同步长度，w28=复帧数量，w29=首SFID。
 * w30位[7:0]=最近SFID，[8]=PN已对齐，[17:9]=预期下个SFID。
 * STATUS位6=PN参考已对齐；比较载荷计数只统计已对齐部分。
 * 统计使用PS扩展64位计数；不是RF验收状态。 */
u32 rx_receiver_init(void);
u32 rx_receiver_apply(const rx_receiver_config_t *cfg);
u32 rx_receiver_stop(void);
u8 rx_receiver_active(void);
void rx_receiver_service(void);
void rx_receiver_snapshot(u32 words[RXR_CFG_WORDS]);
/* main.c实现：只读前置核验、RX2调谐回读；不得在这里自动开启TX。 */
u32 board_receiver_can_apply(const rx_receiver_config_t *cfg);
u32 board_receiver_tune(const rx_receiver_config_t *cfg);
/* 非阻塞网络输出，失败会显式计丢包；无PC仍继续PL比较/本地统计。 */
int udp_send_receiver_bits(const u32 *words,u16 count,u32 epoch,u32 sequence,
                           u32 bit_offset,u32 rate,u32 status);
#endif
