#include "dma_tx_bit.h"
#include "xaxidma.h"
#include "xparameters.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "sleep.h"
#include "xtime_l.h"

#define TXBE_BASE              XPAR_TX_BIT_ENGINE_0_BASEADDR
#define TXBE_REG_CTRL          0x00U
#define TXBE_REG_STATUS        0x04U
#define TXBE_REG_BIT_COUNT     0x08U
#define TXBE_REG_REPEAT_COUNT  0x0CU
#define TXBE_REG_PROFILE       0x10U
#define TXBE_REG_CHANNEL       0x14U
#define TXBE_REG_BIT_RATE      0x18U
#define TXBE_REG_BITS_SENT     0x1CU
#define TXBE_REG_FRAMES_DONE   0x20U
#define TXBE_REG_ERROR_CODE    0x24U
#define TXBE_REG_VERSION       0x28U
#define TXBE_REG_RATE_WORD     0x2CU

#define TXBE_CTRL_START        0x01U
#define TXBE_CTRL_STOP         0x02U
#define TXBE_CTRL_CLEAR        0x04U
#define TXBE_ST_BUSY           0x01U
#define TXBE_ST_DONE           0x02U
#define TXBE_ST_UNDERRUN       0x04U
#define TXBE_ST_ERROR          0x08U
/* AXIS/tx_bit_engine clock is the 61.44 MHz LVDS sample clock. */
#define TXBE_AXIS_CLK_HZ       61440000U
#define TXBE_COMMAND_SETTLE_US 10U
#define TXBE_QUIESCE_TIMEOUT_US 5000U
#define TXBE_DMA_RESET_TIMEOUT_US 100000U
#define TXBE_SUBMIT_TIMEOUT_US 5000U
#define TXBE_REPLAY_BYTES      16384U

static XAxiDma tx_dma;
static u8 tx_initialized;
static volatile u8 tx_active;
static u8 tx_wait_for_arm;
static XTime tx_arm_start;
static u32 tx_software_error;
static u8 tx_dma_faulted;
static const u8 *tx_buffer;
static u32 tx_bytes;
static u32 tx_valid_bits;
static u32 tx_repeat_remaining;
static u32 tx_profile;
static u32 tx_channel;
static u32 tx_bit_rate;
static u32 tx_rate_word;
static u32 tx_completed_frames;

static u32 tx_dma_status(void)
{
    return Xil_In32(tx_dma.RegBase + XAXIDMA_TX_OFFSET + XAXIDMA_SR_OFFSET);
}

static void txbe_write(u32 offset, u32 value)
{
    Xil_Out32(TXBE_BASE + offset, value);
}

static u32 txbe_read(u32 offset)
{
    return Xil_In32(TXBE_BASE + offset);
}

static u32 txbe_rate_word(u32 bit_rate)
{
    return (u32)((((u64)bit_rate << 32) + (TXBE_AXIS_CLK_HZ / 2U)) /
                 TXBE_AXIS_CLK_HZ);
}

/* A DMA reset is normally completed in a few cycles.  Keep the wait bounded
 * so a wedged AXI channel cannot permanently stall the control loop. */
static int tx_dma_reset_wait(void)
{
    u32 timeout;

    XAxiDma_Reset(&tx_dma);
    for (timeout = 0U; timeout < TXBE_DMA_RESET_TIMEOUT_US; timeout++) {
        if (XAxiDma_ResetIsDone(&tx_dma)) {
            tx_dma_faulted = 0U;
            return XST_SUCCESS;
        }
        usleep(1U);
    }
    tx_dma_faulted = 1U;
    tx_software_error = DMA_TX_BIT_ERR_RESET;
    return XST_FAILURE;
}

static int txbe_wait_quiet(u32 timeout_us)
{
    u32 timeout;

    for (timeout = 0U; timeout < timeout_us; timeout++) {
        u32 status = txbe_read(TXBE_REG_STATUS);
        if ((status & (TXBE_ST_BUSY | TXBE_ST_DONE |
                       TXBE_ST_UNDERRUN | TXBE_ST_ERROR)) == 0U &&
            txbe_read(TXBE_REG_BITS_SENT) == 0U)
            return XST_SUCCESS;
        usleep(1U);
    }
    return XST_FAILURE;
}

static int dma_submit_frame(void)
{
    int status;
    u32 timeout;
    u32 bit_rate_readback;
    u32 rate_word_readback;

    txbe_write(TXBE_REG_CTRL, TXBE_CTRL_CLEAR);
    /* CLEAR crosses from AXI clock to data_clk. When the engine is already
     * idle its status is zero both before and after CLEAR, so status polling
     * alone cannot acknowledge the command. Give the toggle synchronizer
     * deterministic time to deliver CLEAR before a later START; otherwise
     * both pulses can coincide and CLEAR (which has priority) swallows START. */
    usleep(TXBE_COMMAND_SETTLE_US);
    for (timeout = 0U; timeout < TXBE_SUBMIT_TIMEOUT_US; timeout++) {
        if ((txbe_read(TXBE_REG_STATUS) &
             (TXBE_ST_BUSY | TXBE_ST_DONE | TXBE_ST_UNDERRUN | TXBE_ST_ERROR)) == 0U &&
            txbe_read(TXBE_REG_BITS_SENT) == 0U)
            break;
        usleep(1U);
    }
    if (timeout == TXBE_SUBMIT_TIMEOUT_US)
        return XST_FAILURE;

    txbe_write(TXBE_REG_BIT_COUNT, tx_valid_bits);
    txbe_write(TXBE_REG_REPEAT_COUNT, tx_repeat_remaining);
    txbe_write(TXBE_REG_PROFILE, tx_profile);
    txbe_write(TXBE_REG_CHANNEL, tx_channel);
    txbe_write(TXBE_REG_BIT_RATE, tx_bit_rate);
    txbe_write(TXBE_REG_RATE_WORD, tx_rate_word);
    bit_rate_readback = txbe_read(TXBE_REG_BIT_RATE);
    rate_word_readback = txbe_read(TXBE_REG_RATE_WORD);
    if (bit_rate_readback != tx_bit_rate || rate_word_readback != tx_rate_word ||
        txbe_read(TXBE_REG_BIT_COUNT) != tx_valid_bits ||
        txbe_read(TXBE_REG_REPEAT_COUNT) != tx_repeat_remaining ||
        txbe_read(TXBE_REG_PROFILE) != tx_profile ||
        txbe_read(TXBE_REG_CHANNEL) != tx_channel)
        return XST_FAILURE;

    Xil_DCacheFlushRange((UINTPTR)tx_buffer, tx_bytes);
    status = XAxiDma_SimpleTransfer(&tx_dma, (UINTPTR)tx_buffer, tx_bytes,
                                    XAXIDMA_DMA_TO_DEVICE);
    if (status != XST_SUCCESS)
        return status;

    /* The MM2S/FIFO path starts first so the BIT engine never leads the data. */
    txbe_write(TXBE_REG_CTRL, TXBE_CTRL_START);
    tx_wait_for_arm = 1U;
    XTime_GetTime(&tx_arm_start);
    return XST_SUCCESS;
}

int dma_tx_bit_init(void)
{
    XAxiDma_Config *cfg;
    u32 version;

    tx_initialized = tx_active = tx_wait_for_arm = 0U;
    tx_software_error = 0U;
    cfg = XAxiDma_LookupConfig(XPAR_AXI_DMA_TX_0_DEVICE_ID);
    if (cfg == NULL)
        return XST_FAILURE;
    if (XAxiDma_CfgInitialize(&tx_dma, cfg) != XST_SUCCESS)
        return XST_FAILURE;
    if (XAxiDma_HasSg(&tx_dma) || !tx_dma.HasMm2S ||
        tx_dma.HasS2Mm || tx_dma.MicroDmaMode)
        return XST_FAILURE;

    XAxiDma_IntrDisable(&tx_dma, XAXIDMA_IRQ_ALL_MASK,
                        XAXIDMA_DMA_TO_DEVICE);
    if (tx_dma_reset_wait() != XST_SUCCESS) {
        xil_printf("[txbit] DMA reset timeout during init\r\n");
        return XST_FAILURE;
    }

    version = txbe_read(TXBE_REG_VERSION);
    if (version != 0x00010200U) {
        xil_printf("[txbit] bad engine version=0x%08x\r\n", (unsigned)version);
        return XST_FAILURE;
    }

    txbe_write(TXBE_REG_CTRL, TXBE_CTRL_STOP);
    txbe_write(TXBE_REG_CTRL, TXBE_CTRL_CLEAR);
    tx_initialized = 1U;
    tx_active = 0U;
    tx_wait_for_arm = 0U;
    tx_completed_frames = 0U;
    xil_printf("[txbit] ready dma=%u base=0x%08x engine=0x%08x\r\n",
               (unsigned)XPAR_AXI_DMA_TX_0_DEVICE_ID,
               (unsigned)XPAR_AXI_DMA_TX_0_BASEADDR,
               (unsigned)TXBE_BASE);
    return XST_SUCCESS;
}

int dma_tx_bit_start(const u8 *data, u32 valid_bits, u32 repeat_count,
                     u32 profile, u32 channel, u32 bit_rate)
{
    u32 bytes;

    if (!tx_initialized || data == NULL || valid_bits == 0U ||
        bit_rate == 0U || bit_rate >= TXBE_AXIS_CLK_HZ)
        return XST_INVALID_PARAM;
    if (tx_dma_faulted) return XST_FAILURE;
    if (tx_active)
        return XST_DEVICE_BUSY;
    /* 不先做 valid_bits+7，避免无符号溢出变成小传输。DRE=0需8B对齐。 */
    bytes = (valid_bits >> 3) + ((valid_bits & 7U) != 0U);
    if (bytes == 0U || bytes > tx_dma.TxBdRing.MaxTransferLen ||
        (!tx_dma.TxBdRing.HasDRE &&
         ((UINTPTR)data & (tx_dma.TxBdRing.DataWidth - 1U)) != 0U))
        return XST_INVALID_PARAM;
    /* Repeated jobs are captured once in the 4096x32 PL BRAM. Single-shot
     * jobs retain the original streaming path and may be larger. */
    if (repeat_count != 0U && bytes > TXBE_REPLAY_BYTES) {
        xil_printf("[txbit] repeat frame too large: %u > %u bytes\r\n",
                   (unsigned)bytes, (unsigned)TXBE_REPLAY_BYTES);
        return XST_INVALID_PARAM;
    }

    tx_buffer = data;
    tx_bytes = bytes;
    tx_valid_bits = valid_bits;
    tx_repeat_remaining = repeat_count;
    tx_profile = profile;
    tx_channel = channel;
    tx_bit_rate = bit_rate;
    tx_rate_word = txbe_rate_word(bit_rate);
    tx_completed_frames = 0U;
    tx_software_error = 0U;

    xil_printf("[txbit] cfg rate_req=%u qword=0x%08x bits=%u repeat=%u\r\n",
               (unsigned)tx_bit_rate, (unsigned)tx_rate_word,
               (unsigned)tx_valid_bits,
               (unsigned)tx_repeat_remaining);

    if (dma_submit_frame() != XST_SUCCESS) {
        dma_tx_bit_clear();
        return XST_FAILURE;
    }
    tx_active = 1U;
    return XST_SUCCESS;
}

void dma_tx_bit_stop(void)
{
    int reset_status;

    if (!tx_initialized)
        return;
    txbe_write(TXBE_REG_CTRL, TXBE_CTRL_STOP);
    usleep(TXBE_COMMAND_SETTLE_US);
    /* MM2S已空闲也可能有字留在下游FIFO；中途STOP必须触发软复位清空FIFO。 */
    if (tx_active || tx_dma_faulted || XAxiDma_Busy(&tx_dma, XAXIDMA_DMA_TO_DEVICE) ||
        (tx_dma_status() & XAXIDMA_ERR_ALL_MASK)) {
        reset_status = tx_dma_reset_wait();
        if (reset_status != XST_SUCCESS)
            xil_printf("[txbit] DMA reset timeout during STOP\r\n");
    }
    tx_active = 0U;
    tx_wait_for_arm = 0U;
}

void dma_tx_bit_clear(void)
{
    dma_tx_bit_stop();
    if (tx_initialized) {
        txbe_write(TXBE_REG_CTRL, TXBE_CTRL_CLEAR);
        usleep(TXBE_COMMAND_SETTLE_US);
        if (!tx_dma_faulted) tx_software_error = 0U;
    }
}

int dma_tx_bit_quiesce(void)
{
    int result = XST_SUCCESS;

    if (!tx_initialized) {
        tx_active = 0U;
        tx_wait_for_arm = 0U;
        return tx_dma_faulted ? XST_FAILURE : XST_SUCCESS;
    }

    /* STOP first prevents a repeat frame from being re-armed while the
     * software is changing RF/waveform registers. */
    txbe_write(TXBE_REG_CTRL, TXBE_CTRL_STOP);
    usleep(TXBE_COMMAND_SETTLE_US);

    /* 配置事务必须从空FIFO开始，不能只看DMA Idle（数据可能已搬入FIFO）。 */
    if (tx_dma_reset_wait() != XST_SUCCESS) {
        xil_printf("[txbit] DMA reset timeout during quiesce\r\n");
        result = XST_FAILURE;
    }

    /* CLEAR removes sticky DONE/ERROR and bits_sent, making the next START
     * independent of the previous frame. */
    txbe_write(TXBE_REG_CTRL, TXBE_CTRL_CLEAR);
    usleep(TXBE_COMMAND_SETTLE_US);
    if (txbe_wait_quiet(TXBE_QUIESCE_TIMEOUT_US) != XST_SUCCESS) {
        /* One extra CLEAR handles a command that was sampled on the edge of
         * the first settle interval. */
        txbe_write(TXBE_REG_CTRL, TXBE_CTRL_CLEAR);
        usleep(TXBE_COMMAND_SETTLE_US);
        if (txbe_wait_quiet(TXBE_QUIESCE_TIMEOUT_US) != XST_SUCCESS) {
            xil_printf("[txbit] quiesce timeout st=0x%08x sent=%u\r\n",
                       (unsigned)txbe_read(TXBE_REG_STATUS),
                       (unsigned)txbe_read(TXBE_REG_BITS_SENT));
            result = XST_FAILURE;
        }
    }

    tx_active = 0U;
    tx_wait_for_arm = 0U;
    tx_completed_frames = 0U;
    if (result == XST_SUCCESS) tx_software_error = 0U;
    return result;
}

void dma_tx_bit_service(void)
{
    u32 status;
    u32 sent;
    u32 dma_status;
    XTime now;

    if (!tx_initialized || !tx_active)
        return;

    /* PL循环回放时DMA空闲是正常的；但DMA总线错误不能只看PL BUSY而漏报。 */
    dma_status = tx_dma_status();
    if (dma_status & (XAXIDMA_ERR_ALL_MASK | XAXIDMA_HALTED_MASK)) {
        tx_software_error = DMA_TX_BIT_ERR_DMA |
            (dma_status & (XAXIDMA_ERR_ALL_MASK | XAXIDMA_HALTED_MASK));
        xil_printf("[txbit] MM2S bus error sr=0x%08x; stopping\r\n", (unsigned)dma_status);
        dma_tx_bit_stop();
        return;
    }
    status = txbe_read(TXBE_REG_STATUS);
    if (tx_wait_for_arm) {
        sent = txbe_read(TXBE_REG_BITS_SENT);
        if ((status & (TXBE_ST_UNDERRUN | TXBE_ST_ERROR)) != 0U) {
            tx_software_error = DMA_TX_BIT_ERR_ENGINE | txbe_read(TXBE_REG_ERROR_CODE);
            xil_printf("[txbit] arm error st=0x%08x code=%u\r\n",
                       (unsigned)status,
                       (unsigned)txbe_read(TXBE_REG_ERROR_CODE));
            dma_tx_bit_stop();
            return;
        }
        if ((status & TXBE_ST_BUSY) != 0U ||
            ((status & TXBE_ST_DONE) != 0U && sent == tx_valid_bits)) {
            tx_wait_for_arm = 0U;
        } else {
            XTime_GetTime(&now);
            if (now - tx_arm_start >= COUNTS_PER_SECOND / 10U) {
                tx_software_error = DMA_TX_BIT_ERR_ARM;
                xil_printf("[txbit] START handshake timeout st=0x%08x sent=%u\r\n",
                           (unsigned)status, (unsigned)sent);
                dma_tx_bit_stop();
                return;
            }
            /* Keep the main loop/UDP control path responsive while waiting
             * for the START toggle to cross into the data clock domain. */
            usleep(1U);
            return;
        }
    }

    if ((status & (TXBE_ST_UNDERRUN | TXBE_ST_ERROR)) != 0U) {
        tx_software_error = DMA_TX_BIT_ERR_ENGINE | txbe_read(TXBE_REG_ERROR_CODE);
        sent = txbe_read(TXBE_REG_BITS_SENT);
        xil_printf("[txbit] engine error st=0x%08x code=%u sent=%u/%u\r\n",
                   (unsigned)status,
                   (unsigned)txbe_read(TXBE_REG_ERROR_CODE),
                   (unsigned)sent, (unsigned)tx_valid_bits);
        dma_tx_bit_stop();
        return;
    }

    if ((status & TXBE_ST_DONE) == 0U)
        return;

    sent = txbe_read(TXBE_REG_BITS_SENT);
    if (XAxiDma_Busy(&tx_dma, XAXIDMA_DMA_TO_DEVICE) ||
        sent != tx_valid_bits) {
        tx_software_error = DMA_TX_BIT_ERR_COMPLETION;
        xil_printf("[txbit] completion mismatch dma=%u sent=%u/%u\r\n",
                   (unsigned)XAxiDma_Busy(&tx_dma, XAXIDMA_DMA_TO_DEVICE),
                   (unsigned)sent, (unsigned)tx_valid_bits);
        dma_tx_bit_stop();
        return;
    }

    /* v1.1 performs every repeat inside PL. DONE means the requested finite
     * job is completely finished; an infinite replay remains BUSY until STOP. */
    tx_completed_frames = txbe_read(TXBE_REG_FRAMES_DONE);
    tx_active = 0U;
    xil_printf("[txbit] done bits=%u frames=%u (PL replay)\r\n",
               (unsigned)sent, (unsigned)tx_completed_frames);
}

void dma_tx_bit_get_status(dma_tx_bit_status_t *status)
{
    if (status == NULL)
        return;
    status->engine_status = tx_initialized ? txbe_read(TXBE_REG_STATUS) : 0U;
    status->bits_sent = tx_initialized ? txbe_read(TXBE_REG_BITS_SENT) : 0U;
    status->error_code = tx_software_error |
        (tx_initialized ? txbe_read(TXBE_REG_ERROR_CODE) : 0U);
    status->dma_busy = tx_initialized ?
        (u32)XAxiDma_Busy(&tx_dma, XAXIDMA_DMA_TO_DEVICE) : 0U;
    status->active = tx_active;
    status->completed_frames = tx_initialized ?
        txbe_read(TXBE_REG_FRAMES_DONE) : tx_completed_frames;
    status->configured_bits = tx_initialized ? txbe_read(TXBE_REG_BIT_COUNT) : 0U;
    status->configured_bit_rate = tx_initialized ? txbe_read(TXBE_REG_BIT_RATE) : 0U;
}

u8 dma_tx_bit_is_active(void)
{
    return tx_active;
}
