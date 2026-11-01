/* ============================================================================
 * dma_rx.h —— TX-DAC / RX-ADC I/Q 经 AXI-DMA(S2MM) 采进 DDR
 *
 * 数据通路：物理 RX2/TX2 的逻辑槽0 → rx2r_pack(61.44 MSPS) → FIFO
 *         → axi_dma(S2MM) → S_AXI_HP0 → DDR
 *
 * 依赖（新 XSA 的 xparameters.h，Vivado 跑完 merge_bd.tcl 后生成）：
 *   XPAR_AXI_DMA_0_DEVICE_ID / XPAR_AXI_DMA_0_BASEADDR
 *   XPAR_RX2R_PACK_0_BASEADDR
 *   XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR  (IRQ_F2P[0], GIC SPI 61)
 *
 * rx2r_pack 寄存器（基址 RX2R_PACK_BASE）：
 *   0x00 CTRL    bit0 capture_en（上升沿触发一次捕获）, bit1 one_shot(兼容保留)
 *   0x04 N       本次捕获的 32bit 字数（SINGLE_SLOT=1，不强制偶数）
 *   0x08 STATUS  bit0 done, bit1 backpressure, bit2 armed
 *   0x0C CNT     已写字数
 *   0x10 TS_LO   捕获起点时间戳低 32（监视器采样时钟计数）
 *   0x14 TS_HI   捕获起点时间戳高 32
 *
 * 每样本打包格式：每 32bit 字 = 一个复样本 {Q[15:0], I[15:0]}（12bit 符号扩展）。
 *   buf[k] = {Q0,I0}；真实 RX2 / TX2 位于逻辑槽0。
 *   旧 UDP 协议槽1由 PS 补零，不占 DMA 字数。
 * ==========================================================================*/
#ifndef DMA_RX_H
#define DMA_RX_H

#include "xil_types.h"
#include "xaxidma.h"
#include "xscugic.h"

/* ---- 设备/中断/基址（新 XSA 后由 xparameters.h 提供） ---- */
#ifndef XPAR_AXI_DMA_0_DEVICE_ID
#error "xparameters.h 缺少 XPAR_AXI_DMA_0_DEVICE_ID —— 请先完成 Vivado BD 改造并导出新 XSA"
#endif

#define DMA_RX_DEV_ID    XPAR_AXI_DMA_0_DEVICE_ID
#define DMA_RX_INTR_ID   XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR   /* GIC SPI 61 */
#define RX2R_PACK_BASE   XPAR_RX2R_PACK_0_BASEADDR                /* 0x43C10000 */

/* ---- rx2r_pack 寄存器偏移 ---- */
#define RX2R_CTRL       0x00
#define RX2R_N          0x04
#define RX2R_STATUS     0x08
#define RX2R_CNT        0x0C
#define RX2R_TS_LO      0x10
#define RX2R_TS_HI      0x14
#define RX2R_SOURCE     0x18

/* CTRL 位 */
#define RX2R_CAPTURE_EN  0x1
#define RX2R_ONE_SHOT    0x2
/* STATUS 位 */
#define RX2R_ST_DONE     0x1
#define RX2R_ST_BP       0x2
#define RX2R_ST_ARMED    0x4
#define RX2R_ST_SOURCE_TX 0x8
#define RX2R_ST_TX_SEEN   0x10
#define RX2R_ST_RX_SEEN   0x20

/* SOURCE 值 */
#define RX2R_SOURCE_RX_ADC 0U
#define RX2R_SOURCE_TX_DAC 1U

/* ---- 保留旧协议结构；当前 i1/q1 恒为0 ---- */
typedef struct {
    s16 i0;   /* 逻辑槽0 I */
    s16 q0;   /* 逻辑槽0 Q */
    s16 i1;   /* 兼容空槽 */
    s16 q1;
} __attribute__((packed)) sample_pack_t;

/* ---- API ---- */
XScuGic *dma_rx_gic_instance(void);     /* 返回既有 GIC 实例（eth_link 复用挂 GEM IRQ） */
int  dma_rx_init(void);                 /* 查找并初始化 AXI DMA (S2MM)，复位+关中断 */
int  dma_rx_is_ready(void);
int  dma_rx_gic_init(void);             /* 初始化 GIC + 挂 dma_rx_isr + 开异常（main 调一次） */
void dma_rx_intr_enable(void);
void dma_rx_intr_disable(void);
void dma_rx_isr(void *CallbackRef);     /* IOC/ERROR 回调；错误优先，不在中断中打印 */
int  dma_rx_start(u32 *buf, u32 nwords);/* buf需64B对齐；nwords×4；flush后提交 */
int  dma_rx_wait_done(u32 timeout_us); /* 真实微秒超时，检查错误及完整字数；失败自动恢复 */
/* 仅供前台等待期间预算化收串口；返回非0取消本次采集。不得在回调内操作DMA/RF。 */
void dma_rx_set_wait_hook(u8 (*hook)(void));
int  dma_rx_abort_recover(void);        /* 取消 packer 并复位 S2MM；超时/错误后自恢复 */
u32  dma_rx_hw_status(void);            /* S2MM SR 快照，TXST/UDP诊断用 */
u32  dma_rx_timeout_count(void);
u32  dma_rx_recovery_count(void);
u32  dma_rx_error_count(void);          /* DMA IRQ/帧长错误计数；ISR不打印 */
u32  dma_rx_last_error(void);           /* Last raw S2MM interrupt status */

void rx2r_trigger(u32 nwords);          /* 写 N + capture_en 上升沿触发（先 DMA 就绪再调） */
u32  rx2r_status(void);
void rx2r_read_ts(u32 *lo, u32 *hi);
int  rx2r_set_source(u32 source);       /* 仅空闲时切换；进行中返回BUSY；0=RX ADC,1=TX DAC */
u32  rx2r_get_source(void);

int  dma_rx_get_sample(const u32 *buf, u32 idx, sample_pack_t *s);

#endif /* DMA_RX_H */
