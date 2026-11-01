#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H
#include "xil_types.h"

/* CFGQ+nonce只读查询；CFGA+nonce+38个小端u32，一次响应返回同一主循环的快照。
 * 返回已应用软件缓存及PL任务状态，不在UDP处理里改变RF或执行SPI校准。
 * 原始ADC/射频功率、频谱合格与否仍需实物测量。协议变更必须升级版本。 */
/* BC_TX1/BC_ATT1是历史命名的逻辑槽0，现映射物理TX2；BC_TX2是禁用槽。
 * 保持38字报文兼容现有UI，不把物理通道切换错误地当作启用第二数据流。
 * 实际射频端口选择及启动寄存器核验见rf_channel_profile.h和[rf-channel]日志。 */
#define BOARD_CONFIG_VERSION 9U
#define BOARD_CONFIG_WORDS 38U
#define BOARD_CONFIG_RF_VALID 1024U
#define BOARD_CONFIG_P351 2048U
#define BOARD_CONFIG_HW_QUALIFIED 4096U
#define BOARD_CONFIG_RX_IQ_FIXED 8192U
#define BOARD_CONFIG_SINGLE_SLOT 16384U
#define BOARD_CONFIG_RX_DSP 32768U
#define BOARD_CONFIG_RX_TIMING 65536U
enum {
    BC_VERSION, BC_FLAGS, BC_TX1, BC_TX2, BC_LO_LOW, BC_LO_HIGH, BC_BW,
    BC_FS, BC_ATT1, BC_ATT2, BC_RATE, BC_RATE_WORD, BC_DEV_WORD, BC_OFFSET_WORD,
    BC_AMPLITUDE, BC_LINE, BC_ROLLOFF, BC_RANDOMIZER, BC_DIFFERENTIAL,
    BC_REF_SOURCE, BC_REF_RATE, BC_TASK, BC_BITS, BC_REPEAT, BC_SOURCE,
    BC_FRAMED, BC_SEED, BC_INVERT, BC_SYNC_LOW, BC_SYNC_HIGH, BC_SYNC_BITS,
    BC_FRAME_BYTES, BC_FRAME_COUNT, BC_SFID_START, BC_RAW_BITS,
    BC_ZERO_MDBM, BC_TARGET_MDBM, BC_BOOT_RESULT
};
/* FLAGS：bit0参考有效，bit1上电自动发射编译开关，bit2 PCM使能，bit3 SOQPSK使能，
 * bit4本地PN11配置已知，bit5任务active，bit6任务错误，bit7内部BIST诊断活动。
 * V2新增bit8物理RX2、bit9物理TX2；对应位为0表示物理通道1。报文仍为38字。
 * V3新增bit10：RF配置事务已通过校验，且未因失败锁止；不证明射频频谱合格。
 * P351 V4新增bit11：P351板型，bit12：本固件射频资格保护已放行。
 * P351 V5新增bit13：回读PL的RX I/Q/PN修复能力；不是射频接收验收标志。
 * P351 V6新增bit14：单槽DMA采集能力；UDP仍发送[I0 Q0 0 0]兼容格式。
 * 位流的硬件能力仍由EMIO身份/能力位另行检查；报文总长不变。
 * P351的REF_SOURCE/REF_RATE表示选用的基准输入0/40M或1/10M；
 * AD9361的真实REFCLK始终40M，外部10M通过ADF4002驯服该40M晶振。
 * SOURCE：0无已提交任务，1本地PN11，2上位机下载（内容类型未知）。
 * BOOT_RESULT：0未开启，1待启动，2启动成功，3启动失败；不是当前RF状态。 */
void board_config_snapshot(u32 words[BOARD_CONFIG_WORDS]);
/* RXSQ/RXSA只读状态：bit0外部RX配置已应用；bit1 LVDS训练PASS；
 * bit2 SOURCE寄存器为RX；bit3 PL当前活动源为RX。不是射频检出/解调标志。 */
u32 board_rx_status(void);
#endif
