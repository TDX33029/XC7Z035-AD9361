#include "lvds_calibration.h"
#include "ad9361.h"
#include "parameters.h"
#include "rf_channel_profile.h"
#include "platform.h"
#include "xil_io.h"
#include "xil_printf.h"
#include <string.h>

#if defined(XPAR_LVDS_PRBS_CHECKER_0_S_AXI_BASEADDR)
#define PRBS_BASE XPAR_LVDS_PRBS_CHECKER_0_S_AXI_BASEADDR
#elif defined(XPAR_LVDS_PRBS_CHECKER_0_BASEADDR)
#define PRBS_BASE XPAR_LVDS_PRBS_CHECKER_0_BASEADDR
#else
/* Never touch an unmapped AXI address in TX-only hardware. */
#define PRBS_BASE 0U
#define PRBS_NOT_PRESENT 1
#endif

#define PRBS_CTRL       0x00U
#define PRBS_WINDOW     0x04U
#define PRBS_STATUS     0x08U
#define PRBS_SAMPLES    0x0CU
#define PRBS_ERR_I0     0x10U
#define PRBS_ERR_Q0     0x14U
#define PRBS_ERR_I1     0x18U
#define PRBS_ERR_Q1     0x1CU
#define PRBS_GAP_ERR    0x20U
#define PRBS_OOS        0x24U
#define PRBS_BUILD_ID   0x28U
#define PRBS_RAW0       0x2CU
#define PRBS_RAW1       0x30U
#define PRBS_RAW2       0x34U
#define PRBS_RAW3       0x38U
#define PRBS_RAW4       0x3CU

#define ST_BUSY         (1U << 0)
#define ST_DONE         (1U << 1)
#define ST_LOCK_MASK    (0xFU << 2)
#define ST_IQ_LOCK_MASK ((1U << 2) | (1U << 3)) /* 逻辑槽0的I/Q，物理RX2 */
#define ST_IQ_MODE_MASK (0xFU << 8) /* 两个视图均须锁定ADI device PN模式0 */
#define ST_ANY_ERROR    (1U << 6)
#define ST_GAP_ERROR    (1U << 7)

#define FAST_SAMPLES    65536U
#define FINAL_SAMPLES   1048576U
#define POINT_TIMEOUT_US 10000U

static u32 snap_token;

typedef struct {
    u32 status;
    u32 samples;
    u32 err_i0;
    u32 err_q0;
    u32 err_i1;
    u32 err_q1;
    u32 gap;
    u32 oos;
} prbs_diag_t;

static prbs_diag_t last_diag;

/* P351仅有物理RX2对应逻辑槽0。I/Q解包次序修复后，两视图应按ADI
 * device PN精确匹配；不再忽略I错误或容许1/256残差。该校验只证明
 * 数字接口训练，不等于验证射频前端、接收功率或频谱。 */

static void ctrl_write(u32 offset, u32 value)
{
    Xil_Out32(AXI_CTRL_BASE_ADDR + offset, value);
}

static void fpga_load_tap(u8 lane_mask, u8 tap)
{
    ctrl_write(DELAY_VALUE, tap & 0x1FU);
    ctrl_write(LOAD_EN, lane_mask & 0x7FU);
    udelay(10);
    ctrl_write(LOAD_EN, 0);
}

static u32 prbs_read(u32 offset)
{
    return Xil_In32(PRBS_BASE + offset);
}

static void prbs_write(u32 offset, u32 value)
{
    Xil_Out32(PRBS_BASE + offset, value);
}

static u32 prbs_snapshot(int enabled)
{
    u32 status;
    u32 target;
    unsigned int retry;
    snap_token ^= 1U;
    target = snap_token << 31;
    prbs_write(PRBS_CTRL, (enabled ? 1U : 0U) | 4U);
    for (retry = 0; retry < 1000; retry++) {
        status = prbs_read(PRBS_STATUS);
        if ((status & 0x80000000U) == target)
            return status;
    }
    return prbs_read(PRBS_STATUS);
}

static int prbs_run(u32 window, u32 timeout_us, u32 *total_errors)
{
    u32 status = 0;
    u32 elapsed;
    u32 errors;

    prbs_write(PRBS_CTRL, 0);
    prbs_write(PRBS_WINDOW, window);
    prbs_write(PRBS_CTRL, 2U); /* toggle clear request, checker disabled */
    udelay(5);
    prbs_write(PRBS_CTRL, 1U); /* rising enable starts the window */

    for (elapsed = 0; elapsed < timeout_us; elapsed += 100U) {
        udelay(100);
        status = prbs_snapshot(1);
        if (status & ST_DONE)
            break;
    }
    status = prbs_snapshot(1);
    last_diag.status = status;
    last_diag.samples = prbs_read(PRBS_SAMPLES);
    last_diag.err_i0 = prbs_read(PRBS_ERR_I0);
    last_diag.err_q0 = prbs_read(PRBS_ERR_Q0);
    last_diag.err_i1 = prbs_read(PRBS_ERR_I1);
    last_diag.err_q1 = prbs_read(PRBS_ERR_Q1);
    last_diag.gap = prbs_read(PRBS_GAP_ERR);
    last_diag.oos = prbs_read(PRBS_OOS);
    errors = last_diag.err_i0 + last_diag.err_q0 +
             last_diag.gap + last_diag.oos;
    if (total_errors)
        *total_errors = errors;
    prbs_write(PRBS_CTRL, 0);

    if (!(status & ST_DONE) || (status & ST_BUSY))
        return 0;
    if ((status & ST_IQ_LOCK_MASK) != ST_IQ_LOCK_MASK || (status & ST_IQ_MODE_MASK))
        return 0;
    if ((status & ST_GAP_ERROR) || last_diag.gap != 0 || last_diag.oos != 0)
        return 0;
    if (last_diag.err_i0 != 0 || last_diag.err_q0 != 0)
        return 0;
    if (last_diag.samples < window)
        return 0;
    return 1;
}

static void prbs_print_diag(const char *tag)
{
    u32 raw0 = prbs_read(PRBS_RAW0);
    u32 raw1 = prbs_read(PRBS_RAW1);
    u32 raw2 = prbs_read(PRBS_RAW2);
    u32 raw3 = prbs_read(PRBS_RAW3);
    u32 raw4 = prbs_read(PRBS_RAW4);
    xil_printf("[lvds-cal] %s st=%08x samples=%u locks=%x modes=%02x\r\n",
               tag, (unsigned)last_diag.status,
               (unsigned)last_diag.samples,
               (unsigned)((last_diag.status & ST_LOCK_MASK) >> 2),
               (unsigned)((last_diag.status >> 8) & 0xFFU));
    xil_printf("[lvds-cal] %s err=%u/%u/%u/%u gap=%u oos=%u\r\n",
               tag, (unsigned)last_diag.err_i0,
               (unsigned)last_diag.err_q0,
               (unsigned)last_diag.err_i1,
               (unsigned)last_diag.err_q1,
               (unsigned)last_diag.gap, (unsigned)last_diag.oos);
    xil_printf("[lvds-cal] raw RX0 I/Q (first 5): %03x/%03x %03x/%03x %03x/%03x %03x/%03x %03x/%03x\r\n",
               (unsigned)((raw0 >> 12) & 0xFFFU), (unsigned)(raw0 & 0xFFFU),
               (unsigned)((raw1 >> 12) & 0xFFFU), (unsigned)(raw1 & 0xFFFU),
               (unsigned)((raw2 >> 12) & 0xFFFU), (unsigned)(raw2 & 0xFFFU),
               (unsigned)((raw3 >> 12) & 0xFFFU), (unsigned)(raw3 & 0xFFFU),
               (unsigned)((raw4 >> 12) & 0xFFFU), (unsigned)(raw4 & 0xFFFU));
}

static void set_ad9361_rx_delay(struct ad9361_rf_phy *phy, u8 clk, u8 data)
{
    ad9361_ensm_force_state(phy, ENSM_STATE_ALERT);
    ad9361_spi_write(phy->spi, REG_RX_CLOCK_DATA_DELAY,
                     DATA_CLK_DELAY(clk) | RX_DATA_DELAY(data));
    ad9361_ensm_force_state(phy, ENSM_STATE_FDD);
    udelay(100);
}

/* Choose the passing point with the largest Manhattan distance from a failing
 * point or the edge.  This selects the centre of the broadest 2-D eye. */
static int choose_eye_center(const u8 pass[16][16], u8 *best_clk, u8 *best_data)
{
    int d, c, dd, cc;
    int best_score = 0;
    for (d = 0; d < 16; d++) {
        for (c = 0; c < 16; c++) {
            int score;
            if (!pass[d][c])
                continue;
            score = d + 1;
            if (16-d < score) score = 16-d;
            if (c+1 < score) score = c+1;
            if (16-c < score) score = 16-c;
            for (dd = 0; dd < 16; dd++) {
                for (cc = 0; cc < 16; cc++) {
                    int dist;
                    if (pass[dd][cc]) continue;
                    dist = (dd>d ? dd-d : d-dd) + (cc>c ? cc-c : c-cc);
                    if (dist < score) score = dist;
                }
            }
            if (score > best_score) {
                best_score = score;
                *best_clk = (u8)c;
                *best_data = (u8)d;
            }
        }
    }
    return best_score;
}

static int choose_1d_center(const u8 pass[32], u8 *centre, u8 *width)
{
    int i, start = -1, best_start = -1, best_len = 0;
    for (i = 0; i <= 32; i++) {
        if (i < 32 && pass[i]) {
            if (start < 0) start = i;
        } else if (start >= 0) {
            int len = i - start;
            if (len > best_len) { best_len = len; best_start = start; }
            start = -1;
        }
    }
    if (best_len == 0) return 0;
    *centre = (u8)(best_start + (best_len-1)/2);
    *width = (u8)best_len;
    return 1;
}

int lvds_rx_auto_calibrate(struct ad9361_rf_phy *phy,
                           lvds_cal_result_t *result,
                           int verbose)
{
#ifdef PRBS_NOT_PRESENT
    if (result) memset(result, 0, sizeof(*result));
    if (verbose) xil_printf("[lvds-cal] skipped: TX-only hardware, RX checker absent\r\n");
    return 0;
#endif
    u8 eye[16][16];
    u8 taps[32];
    u8 best_clk = 0, best_data = 0, centre = 0, width = 0;
    u8 old_delay;
    u8 lane, tap;
    u32 errors = 0;
    int score;

    if (!phy || !result)
        return -1;
    memset(result, 0, sizeof(*result));
    if (prbs_read(PRBS_BUILD_ID) != 0x50524253U) {
        xil_printf("[lvds-cal] PRBS checker build ID missing at 0x%08x\r\n", PRBS_BASE);
        return -2;
    }

    old_delay = ad9361_spi_read(phy->spi, REG_RX_CLOCK_DATA_DELAY);
    ad9361_tx_mute(phy, 1);
    ad9361_bist_loopback(phy, 0);
    ad9361_bist_prbs(phy, BIST_INJ_RX);
    mdelay(1);
    if (verbose)
        xil_printf("[lvds-cal] BIST reg=0x%02x (expected 0x09)\r\n",
                   (unsigned)ad9361_spi_read(phy->spi, REG_BIST_CONFIG));
    if (verbose)
        xil_printf("[lvds-cal] eye criterion: RX%u(logical0) I/Q device PN locked, ierr=qerr=gap=oos=0\r\n",
                   (unsigned)RF_RX_PHYSICAL_CHANNEL);

    /* A middle FPGA delay gives the AD9361 16x16 scan a useful initial eye. */
    fpga_load_tap(0x7F, 16);
    memset(eye, 0, sizeof(eye));
    for (best_clk = 0; best_clk < 16; best_clk++) {
        for (best_data = 0; best_data < 16; best_data++) {
            set_ad9361_rx_delay(phy, best_clk, best_data);
            eye[best_data][best_clk] = (u8)prbs_run(FAST_SAMPLES,
                                                   POINT_TIMEOUT_US, 0);
        }
    }
    score = choose_eye_center(eye, &best_clk, &best_data);
    if (verbose) {
        int d, c;
        xil_printf("[lvds-cal] AD9361 RX eye, o=PASS\r\n    0123456789ABCDEF\r\n");
        for (d=0; d<16; d++) {
            xil_printf("%x:  ", d);
            for (c=0; c<16; c++) xil_printf("%c", eye[d][c] ? 'o' : '.');
            xil_printf("\r\n");
        }
    }
    if (score == 0) {
        /* Re-test the configured fallback point and print enough state to
         * distinguish no adc_valid, cadence errors, no lock and real BER. */
        set_ad9361_rx_delay(phy, (u8)((old_delay >> 4) & 0x0FU),
                            (u8)(old_delay & 0x0FU));
        fpga_load_tap(0x7F, 0);
        (void)prbs_run(FAST_SAMPLES, POINT_TIMEOUT_US, &errors);
        prbs_print_diag("fallback");
        goto fail;
    }
    set_ad9361_rx_delay(phy, best_clk, best_data);

    /* First find a common tap, then refine FRAME and each physical data lane. */
    memset(taps, 0, sizeof(taps));
    for (tap=0; tap<32; tap++) {
        fpga_load_tap(0x7F, tap);
        taps[tap] = (u8)prbs_run(FAST_SAMPLES, POINT_TIMEOUT_US, 0);
    }
    if (!choose_1d_center(taps, &centre, &width))
        goto fail;
    fpga_load_tap(0x7F, centre);
    for (lane=0; lane<7; lane++) result->fpga_tap[lane] = centre;
    if (verbose) xil_printf("[lvds-cal] common FPGA tap=%u width=%u\r\n", centre, width);

    for (lane=6; ; lane = (lane==6 ? 0 : lane+1)) {
        memset(taps, 0, sizeof(taps));
        for (tap=0; tap<32; tap++) {
            fpga_load_tap((u8)(1U<<lane), tap);
            taps[tap] = (u8)prbs_run(FAST_SAMPLES, POINT_TIMEOUT_US, 0);
        }
        if (!choose_1d_center(taps, &centre, &width))
            goto fail;
        result->fpga_tap[lane] = centre;
        fpga_load_tap((u8)(1U<<lane), centre);
        if (verbose) xil_printf("[lvds-cal] lane%u tap=%u width=%u\r\n", lane, centre, width);
        if (lane == 5) break;
    }

    if (!prbs_run(FINAL_SAMPLES, 50000U, &errors))
        goto fail;
    if (verbose)
        prbs_print_diag("final");
    result->valid = 1;
    result->rx_clock_delay = best_clk;
    result->rx_data_delay = best_data;
    result->final_samples = FINAL_SAMPLES;
    result->final_errors = errors;
    ad9361_bist_prbs(phy, BIST_DISABLE);
    ad9361_tx_mute(phy, 0);
    xil_printf("[lvds-cal] PASS clk=%u data=%u errors=%u\r\n",
               best_clk, best_data, (unsigned)errors);
    return 0;

fail:
    ad9361_ensm_force_state(phy, ENSM_STATE_ALERT);
    ad9361_spi_write(phy->spi, REG_RX_CLOCK_DATA_DELAY, old_delay);
    ad9361_ensm_force_state(phy, ENSM_STATE_FDD);
    fpga_load_tap(0x7F, 0);
    ad9361_bist_prbs(phy, BIST_DISABLE);
    ad9361_tx_mute(phy, 0);
    xil_printf("[lvds-cal] FAIL; restored RX delay=0x%02x and FPGA tap0\r\n", old_delay);
    return -3;
}
