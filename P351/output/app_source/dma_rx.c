/* ============================================================================
 * dma_rx.c —— TX-DAC / RX-ADC I/Q 经 AXI-DMA(S2MM) 采进 DDR
 *
 * 流程：物理 RX2 / TX2 的逻辑槽0 → rx2r_pack（61.44 MSPS）→ FIFO → S2MM
 *       → HP0 → DDR。中断 s2mm_introut → xlconcat → IRQ_F2P[0] → GIC SPI 61。
 *
 * 采集时序（关键，rx2r_pack 为 capture_en 上升沿触发、单次捕获）：
 *   1) dma_rx_start(buf, N)        —— DMA 先就绪
 *   2) rx2r_trigger(N)             —— 写 N、再写 CTRL=0x3 触发（上升沿）
 *   3) dma_rx_wait_done(...)       —— 等 DMA IOC 中断（FIFO 缓冲由 DMA 拉走）
 *   4) Xil_DCacheInvalidateRange   —— DMA 写 DDR，读前刷 cache
 *   5) 读 STATUS / TS / 解析样本
 *   6) 下次采集：重新走 1~2（rx2r_trigger 内部会先清 CTRL 制造下降沿）
 *
 * 注意：
 *   - 缓冲区需 cache 对齐（DMA 缓冲区 64B 对齐最佳）
 *   - 缓存一致性：DMA 写 DDR，PS 读前 Xil_DCacheInvalidateRange
 *   - DMA 完成后 IOC 置位；错误中断也清（含 Fatal/DecErr/SlvErr）
 * ==========================================================================*/
#include "dma_rx.h"
#include "xparameters.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "xil_exception.h"
#include "xtime_l.h"
#include "sleep.h"

#define DMA_RX_RESET_US 100000U
#define DMA_RX_IRQ_MASK (XAXIDMA_IRQ_IOC_MASK | XAXIDMA_IRQ_ERROR_MASK)

static XAxiDma dma_inst;
static int dma_ready;
int dma_rx_is_ready(void) { return dma_ready; }
static XScuGic dma_gic;
static volatile u32 dma_done = 0;      /* 一次 IOC 完成标志 */
static volatile u32 dma_err  = 0;      /* DMA 错误标志 */
static volatile u32 dma_inflight;
static u32 dma_expected_words;
static volatile u32 dma_error_total = 0;
static volatile u32 dma_last_error_irq = 0;
static volatile u32 dma_timeout_total = 0;
static volatile u32 dma_recovery_total = 0;
static u8 (*dma_wait_hook)(void);
void dma_rx_set_wait_hook(u8 (*hook)(void)) { dma_wait_hook = hook; }

/* eth_link 复用同一个 GIC 实例挂 GEM 中断（SPI 54），避免二次 CfgInitialize 冲掉 DMA */
XScuGic *dma_rx_gic_instance(void) { return &dma_gic; }

/* 使用真实时间，不以 CPU 空循环次数冒充微秒。 */
static int dma_rx_reset_wait(void)
{
    XTime start, now;
    XAxiDma_Reset(&dma_inst);
    XTime_GetTime(&start);
    do {
        if (XAxiDma_ResetIsDone(&dma_inst)) return XST_SUCCESS;
        /* 失效DMA复位也不能饿死屏幕串口；此处只收字节，不递归取消。 */
        if (dma_wait_hook) (void)dma_wait_hook();
        XTime_GetTime(&now);
    } while (now - start < ((u64)COUNTS_PER_SECOND * DMA_RX_RESET_US) / 1000000U);
    return XST_FAILURE;
}

int dma_rx_init(void)
{
    XAxiDma_Config *cfg;
    int status;
    dma_ready = 0;
    dma_inflight = dma_done = dma_err = 0U;

    cfg = XAxiDma_LookupConfig(DMA_RX_DEV_ID);
    if (!cfg) {
        xil_printf("[dma_rx] AXI DMA config not found (dev id=%d)\r\n", DMA_RX_DEV_ID);
        return XST_FAILURE;
    }
    status = XAxiDma_CfgInitialize(&dma_inst, cfg);
    if (status != XST_SUCCESS) {
        xil_printf("[dma_rx] AXI DMA init failed (%d)\r\n", status);
        return status;
    }
    /* BSP/XSA 必须真实一致，不能通过强改 HasSg 掩盖错误平台。 */
    if (XAxiDma_HasSg(&dma_inst) || !dma_inst.HasS2Mm ||
        dma_inst.HasMm2S || dma_inst.MicroDmaMode) {
        xil_printf("[dma_rx] wrong BSP: require simple S2MM-only DMA\r\n");
        return XST_FAILURE;
    }
    /* Vitis重跑PS时PL可能仍在上一次捕获；先停生产者，再复位DMA/FIFO。 */
    Xil_Out32(RX2R_PACK_BASE + RX2R_CTRL, 0U);
    usleep(10U);
    if (dma_rx_reset_wait() != XST_SUCCESS) {
        xil_printf("[dma_rx] reset timeout (100ms); check PL clock/reset/XSA\r\n");
        dma_timeout_total++;
        return XST_FAILURE;
    }
    /* 本实例没有 MM2S 通道，不写不存在的 MM2S 寄存器。 */
    XAxiDma_IntrDisable(&dma_inst, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    xil_printf("[dma_rx] AXI DMA S2MM init OK\r\n");
    dma_ready = 1;
    return XST_SUCCESS;
}

int dma_rx_gic_init(void)
{
    XScuGic_Config *cfg;
    int result;

    cfg = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
    if (!cfg) {
        xil_printf("[dma_rx] GIC config not found\r\n");
        return XST_FAILURE;
    }
    result = XScuGic_CfgInitialize(&dma_gic, cfg, cfg->CpuBaseAddress);
    if (result != XST_SUCCESS) return result;
    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_IRQ_INT,
            (Xil_ExceptionHandler)XScuGic_InterruptHandler, &dma_gic);

    /* 挂 DMA 中断（GIC SPI 61 = IRQ_F2P[0] = s2mm_introut） */
    result = XScuGic_Connect(&dma_gic, DMA_RX_INTR_ID,
            (Xil_ExceptionHandler)dma_rx_isr, &dma_inst);
    if (result != XST_SUCCESS) return result;
    if (dma_ready) XScuGic_Enable(&dma_gic, DMA_RX_INTR_ID);
    Xil_ExceptionEnable();
    xil_printf("[dma_rx] GIC + DMA IRQ mounted (intr id=%d)\r\n", DMA_RX_INTR_ID);
    return XST_SUCCESS;
}

void dma_rx_intr_enable(void)
{
    if (!dma_ready) return;
    if (!dma_inflight) {
        dma_done = 0; dma_err = 0;
        XAxiDma_IntrAckIrq(&dma_inst, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    }
    XAxiDma_IntrEnable(&dma_inst, DMA_RX_IRQ_MASK, XAXIDMA_DEVICE_TO_DMA);
}

void dma_rx_intr_disable(void)
{
    if (!dma_ready) return;
    XAxiDma_IntrDisable(&dma_inst, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
}

/* XScuGic 中断回调：读 IRQ 状态，判断 IOC/错误，用 IntrAckIrq 写 W1C 清除。
 * 注：XAxiDma 无 IntrClear 函数；中断清除用 XAxiDma_IntrAckIrq
 *     （掩码定义在 xaxidma_hw.h：IOC=0x1000, ERROR=0x4000）。 */
void dma_rx_isr(void *CallbackRef)
{
    XAxiDma *d = (XAxiDma *)CallbackRef;
    u32 irq;

    irq = XAxiDma_IntrGetIrq(d, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_IntrAckIrq(d, irq & XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    if (!dma_inflight) return; /* 迟到的旧 IOC 不属于新采集。 */
    if (irq & XAXIDMA_IRQ_ERROR_MASK) {
        dma_err = 1;
        dma_done = 0; /* IOC 与 ERROR 同时出现时，错误必须优先。 */
        /* Keep UART writes out of IRQ context; report these from main instead. */
        dma_error_total++;
        dma_last_error_irq = irq;
    } else if (irq & XAXIDMA_IRQ_IOC_MASK) {
        dma_done = 1;
    }
}

/* 启动一次 S2MM SimpleTransfer，传输 nwords×4 字节 */
int dma_rx_start(u32 *buf, u32 nwords)
{
    int ret;
    if (!dma_ready) return XST_FAILURE;
    /* 当前 DRE=0，DDR AXI 64bit；64B 对齐同时隔离 CPU cache line。 */
    if (!buf || !nwords || ((UINTPTR)buf & 63U) != 0U ||
        nwords > dma_inst.RxBdRing[0].MaxTransferLen / 4U)
        return XST_INVALID_PARAM;
    if (dma_inflight) return XST_DEVICE_BUSY;
    XAxiDma_IntrAckIrq(&dma_inst, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    dma_done = 0; dma_err = 0;
    dma_expected_words = nwords;
    /* 先写回旧脏缓存，避免DMA期间cache淘汰覆盖新DDR数据；完成后由调用方invalidate。 */
    Xil_DCacheFlushRange((UINTPTR)buf, (u32)nwords * 4u);
    dma_inflight = 1U; /* 先登记，再提交；避免短传输 IOC 早于软件登记。 */
    ret = XAxiDma_SimpleTransfer(&dma_inst,
                                  (UINTPTR)buf,
                                  (u32)nwords * 4u,
                                  XAXIDMA_DEVICE_TO_DMA);
    if (ret != XST_SUCCESS) {
        dma_inflight = 0U;
        xil_printf("[dma_rx] start failed sr=0x%08x pack=0x%08x; recovering\r\n",
            (unsigned)dma_rx_hw_status(), (unsigned)rx2r_status());
        dma_rx_abort_recover();
    }
    return ret;
}

u32 dma_rx_hw_status(void)
{
    if (!dma_ready) return XAXIDMA_IRQ_ERROR_MASK;
    return Xil_In32(dma_inst.RegBase + XAXIDMA_RX_OFFSET + XAXIDMA_SR_OFFSET);
}

u32 dma_rx_timeout_count(void) { return dma_timeout_total; }
u32 dma_rx_recovery_count(void) { return dma_recovery_total; }
u32 dma_rx_error_count(void) { return dma_error_total; }
u32 dma_rx_last_error(void) { return dma_last_error_irq; }

/* A timed-out SimpleTransfer must not be left active.  Merely returning from
 * dma_rx_wait_done() leaves both the packer and S2MM busy, so every subsequent
 * SNAP fails and source switching waits forever.  Cancel the PL producer
 * first, reset only the capture DMA, then restore IOC + ERROR interrupts. */
int dma_rx_abort_recover(void)
{
    if (!dma_ready) return XST_FAILURE;

    Xil_Out32(RX2R_PACK_BASE + RX2R_CTRL, 0U);
    usleep(10U); /* 先让停止命令跨入采样域，再清DMA及FIFO。 */
    XAxiDma_IntrDisable(&dma_inst, XAXIDMA_IRQ_ALL_MASK,
                       XAXIDMA_DEVICE_TO_DMA);
    dma_inflight = 0U;
    if (dma_rx_reset_wait() != XST_SUCCESS) {
        xil_printf("[dma_rx] recovery reset timeout sr=0x%08x pack=0x%08x\r\n",
            (unsigned)dma_rx_hw_status(), (unsigned)rx2r_status());
        dma_ready = 0; /* 复位失败后禁止继续向失效 DMA 提交。 */
        return XST_FAILURE;
    }

    dma_done = 0U;
    dma_err = 0U;
    XAxiDma_IntrAckIrq(&dma_inst, XAXIDMA_IRQ_ALL_MASK,
                       XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_IntrEnable(&dma_inst, DMA_RX_IRQ_MASK,
                       XAXIDMA_DEVICE_TO_DMA);
    dma_recovery_total++;
    return XST_SUCCESS;
}

int dma_rx_wait_done(u32 timeout_us)
{
    XTime start, now, serviced;
    u32 settle;
    if (!dma_ready || !dma_inflight) return XST_FAILURE;
    XTime_GetTime(&start);
    serviced = start;
    for (;;) {
        if (dma_err || (dma_rx_hw_status() & XAXIDMA_ERR_ALL_MASK)) {
            xil_printf("[dma_rx] capture error sr=0x%08x pack=0x%08x; recovering\r\n",
                (unsigned)dma_rx_hw_status(), (unsigned)rx2r_status());
            dma_rx_abort_recover();
            return XST_FAILURE;
        }
        if (dma_done) break;
        XTime_GetTime(&now);
        /* 115200 8N1 的64字节FIFO约5.56ms即满；每100us预算化接收。
         * 回调只锁存命令，STOP由调用方返回主循环后执行，避免重入RF事务。 */
        if (dma_wait_hook && now - serviced >= (u64)COUNTS_PER_SECOND / 10000U) {
            serviced = now;
            if (dma_wait_hook()) {
                dma_rx_abort_recover();
                return XST_FAILURE;
            }
        }
        if (now - start >= ((u64)COUNTS_PER_SECOND * timeout_us) / 1000000U) {
            dma_timeout_total++;
            if (dma_timeout_total <= 8U || (dma_timeout_total % 100U) == 0U)
                xil_printf("[dma_rx] capture timeout #%u sr=0x%08x pack=0x%08x cnt=%u; recovering\r\n",
                    (unsigned)dma_timeout_total, (unsigned)dma_rx_hw_status(),
                    (unsigned)rx2r_status(),
                    (unsigned)Xil_In32(RX2R_PACK_BASE + RX2R_CNT));
            dma_rx_abort_recover();
            return XST_FAILURE;
        }
    }
    /* IOC 只证明 DMA 收到了 TLAST；再核实生产者数量，拒绝提前结束的短块。
     * 状态跨时钟同步可能晚于 IOC，允许 16us 收敛，不能立刻误报。 */
    for (settle = 0U; settle < 16U; settle++) {
        if (dma_err || (dma_rx_hw_status() & XAXIDMA_ERR_ALL_MASK)) {
            dma_rx_abort_recover();
            return XST_FAILURE;
        }
        if ((rx2r_status() & (RX2R_ST_DONE | RX2R_ST_ARMED)) == RX2R_ST_DONE &&
            Xil_In32(RX2R_PACK_BASE + RX2R_CNT) == dma_expected_words) {
            dma_inflight = 0U;
            return XST_SUCCESS;
        }
        usleep(1U);
    }
    dma_error_total++;
    xil_printf("[dma_rx] short/stale capture expected=%u count=%u st=0x%08x\r\n",
        (unsigned)dma_expected_words, (unsigned)Xil_In32(RX2R_PACK_BASE + RX2R_CNT),
        (unsigned)rx2r_status());
    dma_rx_abort_recover();
    return XST_FAILURE;
}

/* ---- rx2r_pack 触发（先确保 DMA 已就绪） ----
 * 时序：清 CTRL（下降沿）→ 写 N → 写 CTRL=0x3（capture_en 上升沿触发单次捕获）。
 * 调用方必须先在 dma_rx_start() 里把 DMA 挂好，再调本函数。 */
void rx2r_trigger(u32 nwords)
{
    Xil_Out32(RX2R_PACK_BASE + RX2R_CTRL, 0);        /* 清 capture_en → 下降沿 */
    Xil_Out32(RX2R_PACK_BASE + RX2R_N, nwords);      /* P351单槽：每真实复样本一个32bit字 */
    Xil_Out32(RX2R_PACK_BASE + RX2R_CTRL, RX2R_CAPTURE_EN | RX2R_ONE_SHOT); /* 触发 */
}

u32 rx2r_status(void)
{
    return Xil_In32(RX2R_PACK_BASE + RX2R_STATUS);
}

void rx2r_read_ts(u32 *lo, u32 *hi)
{
    *lo = Xil_In32(RX2R_PACK_BASE + RX2R_TS_LO);
    *hi = Xil_In32(RX2R_PACK_BASE + RX2R_TS_HI);
}

/* DMA事务未结束时拒绝换源；空闲后清capture_en并等待采样域确认，再写SOURCE。 */
int rx2r_set_source(u32 source)
{
    u32 status;
    XTime start, now;

    if (source > RX2R_SOURCE_TX_DAC)
        return XST_INVALID_PARAM;
    if (dma_inflight) return XST_DEVICE_BUSY;
    Xil_Out32(RX2R_PACK_BASE + RX2R_CTRL, 0U);
    XTime_GetTime(&start);
    do {
        status = rx2r_status();
        if (!(status & RX2R_ST_ARMED)) {
            Xil_Out32(RX2R_PACK_BASE + RX2R_SOURCE, source);
            if ((Xil_In32(RX2R_PACK_BASE + RX2R_SOURCE) & 1U) != source)
                return XST_FAILURE;
            xil_printf("[monitor] source=%s reg=0x%08x st=0x%08x\r\n",
                source ? "TX_DAC" : "RX_ADC",
                (unsigned)Xil_In32(RX2R_PACK_BASE + RX2R_SOURCE),
                (unsigned)status);
            return XST_SUCCESS;
        }
        XTime_GetTime(&now);
    } while (now - start < COUNTS_PER_SECOND / 10U);

    xil_printf("[monitor] source switch timeout source=%u st=0x%08x\r\n",
        (unsigned)source, (unsigned)status);
    return XST_FAILURE;
}

u32 rx2r_get_source(void)
{
    return Xil_In32(RX2R_PACK_BASE + RX2R_SOURCE) & 1U;
}

/* P351 1R1T：物理 RX2 在逻辑槽0，一个复样本只占一个 32bit 字。 */
int dma_rx_get_sample(const u32 *buf, u32 idx, sample_pack_t *s)
{
    u32 w0;
    if (!buf || !s) return XST_INVALID_PARAM;
    w0 = buf[idx];
    s->i0 = (s16)(w0 & 0xFFFF);
    s->q0 = (s16)((w0 >> 16) & 0xFFFF);
    s->i1 = 0;
    s->q1 = 0;
    return XST_SUCCESS;
}
