#ifndef P351_CLOCK_PROFILE_H
#define P351_CLOCK_PROFILE_H
/* P351：10 MHz -> ADF4002 -> 40 MHz VCTCXO，AD9361始终声明40 MHz。
 * 用户已确认ADF4002及滤波元件焊接完整。环路调谐方向、锁定时间仍须实测。
 * R=1/N=4是10 MHz与40 MHz的整数比；不是把PL逻辑时钟改为10 MHz。
 * 修改这些参数后须重新编译，并重新做锁定/相噪测试。
 */
#define P351_ADF4002_POPULATED 1U
#define P351_ADF_R_DIVIDER 1U
#define P351_ADF_N_DIVIDER 4U
#define P351_ADF_PD_POSITIVE 1U /* 被动环路、正斜率VCTCXO候选；上板验证调谐方向 */
#define P351_ADF_CP_CODE 0U /* 最小电荷泵电流；RSET=4.7k，禁止直接宣称环路已优化 */
#define P351_CLOCK_REFERENCE_HZ 10000000UL
#define P351_CLOCK_AD9361_HZ 40000000UL
/* 上电默认选择外部10MHz驯服；它不是AD9361 REFCLK的声明频率。
 * AD9361/DMA与通信初始化后才启动异步资格检查。缺参考/失锁保持RF关闭，
 * 不自动回退内部参考、不自动重试或重新发射；内部40MHz仅供显式人工诊断。 */
#define P351_BOOT_REFERENCE_SOURCE 1U /* 0内部40MHz；1外部10MHz驯服40MHz */
#define P351_BOOT_REFERENCE_INPUT_HZ \
    (P351_BOOT_REFERENCE_SOURCE ? P351_CLOCK_REFERENCE_HZ : P351_CLOCK_AD9361_HZ)
#define P351_CLOCK_INPUT_SETTLE_MS 3U
#define P351_CLOCK_MIN_SETTLE_MS 500U
#define P351_CLOCK_LOCK_STABLE_MS 100U
#define P351_CLOCK_TIMEOUT_MS 5000U
#if P351_ADF_R_DIVIDER < 1 || P351_ADF_R_DIVIDER > 16383 || P351_ADF_N_DIVIDER < 1 || P351_ADF_N_DIVIDER > 8191
#error "ADF4002 divider out of range"
#endif
#if P351_CLOCK_REFERENCE_HZ / P351_ADF_R_DIVIDER != P351_CLOCK_AD9361_HZ / P351_ADF_N_DIVIDER
#error "ADF4002 divider ratio does not produce 40MHz from 10MHz"
#endif
#if P351_ADF_PD_POSITIVE > 1 || P351_ADF_CP_CODE > 7
#error "Invalid ADF4002 polarity/current"
#endif
#if P351_BOOT_REFERENCE_SOURCE > 1 || (P351_BOOT_REFERENCE_SOURCE && !P351_ADF4002_POPULATED)
#error "Default external reference requires populated ADF4002"
#endif
#endif
