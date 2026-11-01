#ifndef P351_UPCONVERTER70_H
#define P351_UPCONVERTER70_H
#include <stdint.h>

/* 20260715方案的整机输出，不是历史“9361直出/3725MHz本振”方案。
 * 两次取和频：70+1430=1500 MHz；1500+LO2=最终RF。
 * 本文件只验证/规划，不写SPI，不用频综锁定位冒充实测RF或功率。
 */
#define P351_UP70_IF_KHZ 70000U
#define P351_UP70_LO1_KHZ 1430000U
#define P351_UP70_MID_KHZ 1500000U
#define P351_UP70_RF_STEP_KHZ 100U

static inline int p351_up70_frequency_valid(uint32_t khz)
{
    return (khz % P351_UP70_RF_STEP_KHZ == 0U) &&
        ((khz>=5091000U && khz<=5150000U) ||
         (khz>=5925000U && khz<=6125000U));
}

/* 负功率以正的毫dBm绝对值传输，避免TJC有符号数进入串口急停扫描。
 * 10000..40000代表-10..-40dBm；0.5dB为现有DSA最小档位。
 * 绝对输出精度仍取决于每端口/频点/调制的实测标定。
 */
static inline int p351_up70_power_valid(uint32_t magnitude_mdbm)
{
    return magnitude_mdbm>=10000U && magnitude_mdbm<=40000U &&
           magnitude_mdbm%500U==0U;
}

static inline uint32_t p351_up70_lo2_khz(uint32_t final_khz)
{
    return p351_up70_frequency_valid(final_khz) ? final_khz-P351_UP70_MID_KHZ : 0U;
}
#endif
