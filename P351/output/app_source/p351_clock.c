/* ADF4002写接口与时钟联锁。硬件没有寄存器读回，不能把发送成功当作芯片应答。
 * 独立PL参考存在/频率窗口 + MUXOUT DLD联合判定，缺参考时不静默回退后重开发射。
 * 3线时钟与LTC2630 DAC共用：PL始终把DAC片选拉高，避免误写调谐DAC。
 */
#include "p351_clock.h"
#include "p351_clock_profile.h"
#include "p351_board.h"
#include "platform.h"
#include "sleep.h"

static u8 clock_state, clock_source;
static u32 began_ms, cp_enabled_ms, stable_ms, test_ms;
static u8 stable_started;

u32 p351_adf_function_word(u8 quiet)
{
    /* DB7 PD polarity；DB6..4 MUXOUT=001(DLD)；DB8 CP三态；
     * DB20..18/17..15两组电荷泵电流；fastlock关闭；quiet用异步掉电DB3。
     * 最低两位10为function latch。所有保留位保持0。
     */
    return 2U | (1U << 4) | ((u32)P351_ADF_PD_POSITIVE << 7) |
            ((u32)P351_ADF_CP_CODE << 15) | ((u32)P351_ADF_CP_CODE << 18) |
            (quiet ? ((1U << 8) | (1U << 3)) : 0U);
}
u32 p351_adf_r_word(void) { return (1UL << 20) | ((u32)P351_ADF_R_DIVIDER << 2); }
u32 p351_adf_n_word(void) { return ((u32)P351_ADF_N_DIVIDER << 8) | 1U; }

static void adf_write(u32 word)
{
    int bit;
    gpio_set_value(P351_GPIO_CLK_LE, 0);
    gpio_set_value(P351_GPIO_CLK_SCK, 0);
    for (bit = 23; bit >= 0; --bit) {
        gpio_set_value(P351_GPIO_CLK_DATA, (word >> bit) & 1U);
        usleep(1); /* >=10ns setup；约500kHz，远低于器件上限。 */
        gpio_set_value(P351_GPIO_CLK_SCK, 1);
        usleep(1); /* >=25ns high；保持DATA不变。 */
        gpio_set_value(P351_GPIO_CLK_SCK, 0);
    }
    usleep(1);
    gpio_set_value(P351_GPIO_CLK_LE, 1);
    usleep(1); /* >=20ns LE pulse */
    gpio_set_value(P351_GPIO_CLK_LE, 0);
    gpio_set_value(P351_GPIO_CLK_DATA, 0);
}

u32 p351_clock_inputs(void)
{
    return (gpio_get_value(P351_GPIO_CLK_LOCK) ? 1U : 0U) |
           (gpio_get_value(P351_GPIO_REF_PRESENT) ? 2U : 0U) |
           (gpio_get_value(P351_GPIO_REF_FREQUENCY_OK) ? 4U : 0U) |
           (gpio_get_value(P351_GPIO_REF_QUALIFIED) ? 8U : 0U);
}

void p351_clock_init(void)
{
    clock_source = 0U; clock_state = P351_CLOCK_INTERNAL; stable_started = 0U;
    gpio_set_value(P351_GPIO_TRX2_TX_NRX, 0);
    gpio_set_value(P351_GPIO_REF_ARMED, 0);
    gpio_set_value(P351_GPIO_REF_REQUIRED, 0);
    gpio_set_value(P351_GPIO_CLK_SEL, 0); /* U24 IN=0 -> 内部10MHz支路 */
    if (P351_ADF4002_POPULATED)
        adf_write(p351_adf_function_word(1U)); /* 禁止CP无参考时把VCTCXO拉到极限。 */
}

int p351_clock_begin(u8 source, u32 now_ms)
{
    u32 safe_function = p351_adf_function_word(0U) | (1U << 8);
    if (source > 1U) return -1;
    if (p351_clock_busy()) return -2;
    if (source && !P351_ADF4002_POPULATED) return -3;
    /* REQ翻转前先关物理RF。输出REQ的复位脉冲清除旧PL频率窗口，防止陈旧PASS。 */
    gpio_set_value(P351_GPIO_TRX2_TX_NRX, 0);
    gpio_set_value(P351_GPIO_REF_ARMED, 0);
    gpio_set_value(P351_GPIO_REF_REQUIRED, 0);
    if (P351_ADF4002_POPULATED) adf_write(p351_adf_function_word(1U));
    gpio_set_value(P351_GPIO_CLK_SEL, source);
    clock_source = source; began_ms = now_ms; stable_started = 0U;
    if (!source) {
        clock_state = P351_CLOCK_WAIT_INTERNAL;
        return 0;
    }
    gpio_set_value(P351_GPIO_REF_REQUIRED, 1);
    /* 初始化顺序11->10->00->01；F1=0，先三态CP；首N加载重置计数器。 */
    adf_write((safe_function & ~3U) | 3U);
    adf_write(safe_function);
    adf_write(p351_adf_r_word());
    adf_write(p351_adf_n_word());
    /* 主动验证MUXOUT能低/高响应，防止线路悬空/固定高冒充锁定。
     * 保持CP三态、F1=0、PD1=0；尚未ARM，板载TX路径仍关闭。 */
    adf_write((safe_function & ~(7U << 4)) | (7U << 4)); /* DGND */
    test_ms = now_ms;
    clock_state = P351_CLOCK_TEST_LOW;
    return 0;
}

static void clock_fail(void)
{
    gpio_set_value(P351_GPIO_TRX2_TX_NRX, 0);
    adf_write(p351_adf_function_word(1U));
    /* 保留外部选择和REQ=1：禁止缺参考后自动恢复自由振荡RF。 */
    clock_state = P351_CLOCK_FAULT;
    stable_started = 0U;
}
void p351_clock_abort(void) { clock_fail(); }

void p351_clock_service(u32 now_ms)
{
    u32 inputs;
    if (p351_clock_busy() && (u32)(now_ms - began_ms) >= P351_CLOCK_TIMEOUT_MS) {
        clock_fail(); return;
    }
    if (clock_state == P351_CLOCK_TEST_LOW || clock_state == P351_CLOCK_TEST_HIGH) {
        u8 high = clock_state == P351_CLOCK_TEST_HIGH;
        if ((u32)(now_ms - test_ms) < P351_CLOCK_INPUT_SETTLE_MS) return;
        if (!!gpio_get_value(P351_GPIO_CLK_LOCK) != high) { clock_fail(); return; }
        if (!high) {
            adf_write(((p351_adf_function_word(0U) | (1U << 8)) & ~(7U << 4)) | (3U << 4));
            test_ms = now_ms; clock_state = P351_CLOCK_TEST_HIGH; return; /* DVDD */
        }
        adf_write(p351_adf_function_word(0U) | (1U << 8)); /* Restore DLD */
        clock_state = P351_CLOCK_WAIT_INPUT;
    }
    if (clock_state == P351_CLOCK_WAIT_INTERNAL) {
        if ((u32)(now_ms - began_ms) >= P351_CLOCK_MIN_SETTLE_MS)
            clock_state = P351_CLOCK_INTERNAL;
        return;
    }
    if (clock_state == P351_CLOCK_WAIT_INPUT) {
        if ((u32)(now_ms - began_ms) >= P351_CLOCK_TIMEOUT_MS) {
            clock_fail(); return;
        }
        if ((u32)(now_ms - began_ms) < P351_CLOCK_INPUT_SETTLE_MS) return;
        inputs = p351_clock_inputs();
        if ((inputs & 6U) != 6U) return;
        adf_write(p351_adf_function_word(0U));
        cp_enabled_ms = now_ms;
        clock_state = P351_CLOCK_WAIT_LOCK;
    }
    if (clock_state == P351_CLOCK_WAIT_LOCK) {
        inputs = p351_clock_inputs();
        if ((inputs & 6U) != 6U || (u32)(now_ms - began_ms) >= P351_CLOCK_TIMEOUT_MS) {
            clock_fail(); return;
        }
        if ((inputs & 15U) == 15U) {
            if (!stable_started) { stable_ms = now_ms; stable_started = 1U; }
            if ((u32)(now_ms - cp_enabled_ms) >= P351_CLOCK_MIN_SETTLE_MS &&
                (u32)(now_ms - stable_ms) >= P351_CLOCK_LOCK_STABLE_MS)
            {
                gpio_set_value(P351_GPIO_REF_ARMED, 1);
                clock_state = P351_CLOCK_EXTERNAL;
            }
        } else stable_started = 0U;
    } else if (clock_state == P351_CLOCK_EXTERNAL &&
               (p351_clock_inputs() & 15U) != 15U) {
        clock_fail();
    }
}
u8 p351_clock_state(void) { return clock_state; }
u8 p351_clock_source(void) { return clock_source; }
u8 p351_clock_busy(void)
{
    return clock_state == P351_CLOCK_WAIT_INPUT || clock_state == P351_CLOCK_WAIT_LOCK ||
           clock_state == P351_CLOCK_WAIT_INTERNAL || clock_state == P351_CLOCK_TEST_LOW ||
           clock_state == P351_CLOCK_TEST_HIGH;
}
u8 p351_clock_ready(void)
{
    return clock_state == P351_CLOCK_INTERNAL ||
        (clock_state == P351_CLOCK_EXTERNAL && (p351_clock_inputs() & 15U) == 15U);
}
