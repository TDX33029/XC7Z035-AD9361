/* P351板载射频开关控制。与AD9361内部TX门控分离，关闭仍由内部TX+LO负责。 */
#include "p351_board.h"
#include "platform.h"
#include "xil_printf.h"
#include "p351_clock.h"
#include "xtime_l.h"
#include "sleep.h"
extern int board_final_rf_owned(void);
static u8 safety_heartbeat, safety_present;
static XTime safety_fed;

void p351_rf_safety_service(void)
{
    XTime now;
    if(!safety_present) return;
    XTime_GetTime(&now);
    if(now-safety_fed>=(XTime)COUNTS_PER_SECOND/20U) {
        safety_fed=now; safety_heartbeat^=1U;
        gpio_set_value(P351_GPIO_SAFE_HEARTBEAT,safety_heartbeat);
    }
}
u8 p351_rf_safety_fault(void)
{ return safety_present && gpio_get_value(P351_GPIO_SAFE_FAULT); }

int p351_board_init(void)
{
    static const unsigned outputs[] = {
        P351_GPIO_TRX2_TX_NRX, P351_GPIO_RX2_NTRX_RX,
        P351_GPIO_CLK_SCK, P351_GPIO_CLK_DATA, P351_GPIO_CLK_LE, P351_GPIO_CLK_SEL,
        P351_GPIO_REF_REQUIRED, P351_GPIO_REF_ARMED,
        P351_GPIO_SAFE_HEARTBEAT,P351_GPIO_SAFE_ARM,P351_GPIO_SAFE_REQUIRE_LD
    };
    unsigned i;
    gpio_direction(P351_GPIO_IDENTITY, 0);
    if (!gpio_get_value(P351_GPIO_IDENTITY)) {
        xil_printf("[p351] wrong PL identity; refuse P351 GPIO control\r\n");
        return -1;
    }
    gpio_set_value(P351_GPIO_TRX2_TX_NRX, 0);
    gpio_direction(P351_GPIO_SAFE_CAPABLE,0);
    gpio_direction(P351_GPIO_SAFE_READY,0);
    gpio_direction(P351_GPIO_SAFE_FAULT,0);
    safety_present=gpio_get_value(P351_GPIO_SAFE_CAPABLE)!=0;
    if(!safety_present) {
        xil_printf("[p351-safety] missing hardware watchdog; update matching BIT/XSA, RF blocked\r\n");
        return -1;
    }
    gpio_direction(P351_GPIO_REF_CAPABLE, 0);
    if (!gpio_get_value(P351_GPIO_REF_CAPABLE)) {
        xil_printf("[p351] reference supervisor/DAC isolation missing; update matching BIT/XSA\r\n");
        return -1; /* 不向共享时钟/DAC串行线发送任何命令。 */
    }
    gpio_direction(P351_GPIO_RX_IQ_CAPABLE, 0);
    gpio_direction(P351_GPIO_SINGLE_SLOT, 0);
    gpio_direction(P351_GPIO_RX_DSP, 0);
    gpio_direction(P351_GPIO_RX_TIMING, 0); /* EMIO31输入能力位；EMIO25保留给参考ARM输出。 */
    if (!p351_single_slot_capable()) {
        xil_printf("[p351] single-slot DMA capability missing; require V6 receiver PL/XSA\r\n");
        return -1;
    }
    if (!p351_rx_iq_capable()) {
        xil_printf("[p351] RX I/Q repair capability missing; update matching V5 BIT/XSA\r\n");
        return -1; /* 不接受保留旧RX解包/PN补偿的位流。 */
    }
    for (i = 0; i < sizeof(outputs)/sizeof(outputs[0]); ++i) {
        gpio_set_value(outputs[i], 0); /* 先预置输出，避免方向切换时产生LE脉冲。 */
        gpio_direction(outputs[i], 1);
    }
    gpio_direction(P351_GPIO_TRX2_READBACK, 0);
    gpio_direction(P351_GPIO_RX2_READBACK, 0);
    gpio_direction(P351_GPIO_CLK_LOCK, 0);
    gpio_direction(P351_GPIO_REF_PRESENT, 0);
    gpio_direction(P351_GPIO_REF_FREQUENCY_OK, 0);
    gpio_direction(P351_GPIO_REF_QUALIFIED, 0);
    p351_clock_init();
    safety_heartbeat=0U; XTime_GetTime(&safety_fed);
    /* U58=1选择独立RX2接口；U57=0断开TX2的PA到TRX2接口路径。 */
    gpio_set_value(P351_GPIO_RX2_NTRX_RX, 1);
    xil_printf("[p351] RX2 separate port; TX2 muted; REFCLK=40MHz VCTCXO; RF-qualified=%u\r\n",
               (unsigned)P351_RF_HARDWARE_QUALIFIED);
#if P351_MANUAL_RF_TEST
    xil_printf("[manual-rf] USER-AUTHORIZED FULL RANGE; AD attenuation 0..89.75dB, default89.75dB, no auto-TX; protect RF loads\r\n");
#endif
    return p351_rf_route(0);
}

u8 p351_rx_iq_capable(void)
{
    return gpio_get_value(P351_GPIO_RX_IQ_CAPABLE) ? 1U : 0U;
}

u8 p351_single_slot_capable(void)
{
    return gpio_get_value(P351_GPIO_SINGLE_SLOT) ? 1U : 0U;
}

u8 p351_rx_dsp_capable(void)
{
    return gpio_get_value(P351_GPIO_RX_DSP) ? 1U : 0U;
}

u8 p351_rx_timing_capable(void)
{
    return gpio_get_value(P351_GPIO_RX_TIMING) ? 1U : 0U;
}

int p351_rf_route(u8 transmit)
{
    gpio_set_value(P351_GPIO_TRX2_TX_NRX, 0);
    if (transmit && !P351_RF_HARDWARE_QUALIFIED)
        return -1;
    gpio_set_value(P351_GPIO_RX2_NTRX_RX, 1);
    if(transmit) {
        if(!safety_present) return -1;
        // 只有显式新发射事务可重新武装；主循环绝不自动恢复RF请求。
        gpio_set_value(P351_GPIO_SAFE_ARM,0); usleep(2);
        if(gpio_get_value(P351_GPIO_SAFE_FAULT)) return -1;
        gpio_set_value(P351_GPIO_SAFE_REQUIRE_LD,board_final_rf_owned()?1:0);
        safety_heartbeat^=1U;
        gpio_set_value(P351_GPIO_SAFE_HEARTBEAT,safety_heartbeat);
        XTime_GetTime(&safety_fed);
        gpio_set_value(P351_GPIO_SAFE_ARM,1); usleep(2);
        if(!gpio_get_value(P351_GPIO_SAFE_READY)) return -1;
    }
    gpio_set_value(P351_GPIO_TRX2_TX_NRX, transmit ? 1 : 0);
    if (gpio_get_value(P351_GPIO_TRX2_READBACK) != !!transmit ||
        !gpio_get_value(P351_GPIO_RX2_READBACK)) {
        gpio_set_value(P351_GPIO_TRX2_TX_NRX, 0);
        return -1;
    }
    return 0;
}
