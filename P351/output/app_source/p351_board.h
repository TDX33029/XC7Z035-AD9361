#ifndef P351_BOARD_H
#define P351_BOARD_H
#include "xil_types.h"
/* 默认禁止射频发射。确认Bank电压、DDR、接口校准及外部负载后才能改为1。
 * 不是功能开关：不能以改此宏代替板级验收。原7020工程不使用此文件。 */
#ifndef P351_RF_HARDWARE_QUALIFIED
#define P351_RF_HARDWARE_QUALIFIED 0
#endif
/* 人工射频联调只由独立Vitis构建配置启用；不代表实物验收。
 * 正式保护构建默认0。人工直发可设置0～89.75dB，不使用屏幕功放增益模型；
 * 仍禁止上电自动发射，默认89.75dB。低衰减须由操作员确认实体负载/仪器保护。 */
#ifndef P351_MANUAL_RF_TEST
#define P351_MANUAL_RF_TEST 0
#endif
#define P351_MANUAL_RF_MIN_ATTEN_MDB 0U
#if P351_MANUAL_RF_TEST && !P351_RF_HARDWARE_QUALIFIED
#error "Manual RF build requires explicit transmit permission"
#endif
#define P351_GPIO_TRX2_TX_NRX      (54U + 12U)
#define P351_GPIO_SAFE_HEARTBEAT   (54U + 8U)
#define P351_GPIO_SAFE_ARM         (54U + 9U)
#define P351_GPIO_SAFE_REQUIRE_LD  (54U + 10U)
#define P351_GPIO_SAFE_FAULT       (54U + 8U)
#define P351_GPIO_SAFE_READY       (54U + 9U)
#define P351_GPIO_SAFE_CAPABLE     (54U + 10U)
#define P351_GPIO_RX2_NTRX_RX      (54U + 13U)
#define P351_GPIO_TRX2_READBACK    (54U + 14U)
#define P351_GPIO_RX2_READBACK     (54U + 15U)
#define P351_GPIO_CLK_SCK          (54U + 16U)
#define P351_GPIO_CLK_DATA         (54U + 17U)
#define P351_GPIO_CLK_LE           (54U + 18U)
#define P351_GPIO_CLK_SEL          (54U + 19U)
#define P351_GPIO_CLK_LOCK         (54U + 20U)
#define P351_GPIO_REF_PRESENT      (54U + 21U)
#define P351_GPIO_REF_FREQUENCY_OK (54U + 22U)
#define P351_GPIO_REF_QUALIFIED    (54U + 23U)
#define P351_GPIO_REF_REQUIRED     (54U + 24U)
#define P351_GPIO_REF_ARMED        (54U + 25U)
#define P351_GPIO_REF_CAPABLE      (54U + 29U)
#define P351_GPIO_RX_IQ_CAPABLE    (54U + 28U)
#define P351_GPIO_SINGLE_SLOT      (54U + 27U)
#define P351_GPIO_RX_DSP           (54U + 26U)
#define P351_GPIO_RX_TIMING        (54U + 31U)
#define P351_GPIO_IDENTITY         (54U + 30U)
int p351_board_init(void);
int p351_rf_route(u8 transmit);
/* EMIO同一bit的输出数据和独立输入端可分别接心跳/状态；不增加板外引脚。 */
void p351_rf_safety_service(void);
u8 p351_rf_safety_fault(void);
u8 p351_rx_iq_capable(void);
u8 p351_single_slot_capable(void);
u8 p351_rx_timing_capable(void);
u8 p351_rx_dsp_capable(void);
#endif
