#ifndef P351_IF70_PROFILE_H
#define P351_IF70_PROFILE_H

#include <stdint.h>
#include "boot_pcm_fm_profile.h"

/* 最终前面板的唯一射频约定：TRX2调制中心70MHz、目标平均功率-30dBm。
 * 下面是端口实测标定入口，不是AD9361数据手册的典型输出值。
 * PCM/FM沿用上电profile的实测参数；SOQPSK必须单独填写，不能借用PCM功率。
 * 标定时保持同一数字幅度、滤波带宽、端口和负载；CALIBRATED=0始终拒绝屏幕发射。
 */
#define P351_IF70_CENTER_HZ UINT64_C(70000000)
#define P351_IF70_TARGET_MDBM (-30000)
#define P351_IF70_AMPLITUDE 26214U
#define P351_IF70_BANDWIDTH_HZ 56000000U
#define P351_IF70_SOQ_POWER_CALIBRATED 0U
#define P351_IF70_SOQ_ZERO_MDBM 0 /* 未标定占位；不是0dB衰减时真实功率 */
#define P351_IF70_SAFE_ATTEN_MDB 89750U

#if P351_IF70_SOQ_POWER_CALIBRATED > 1U
#error "SOQPSK power calibration flag must be zero or one"
#endif
#if P351_IF70_SOQ_POWER_CALIBRATED && \
    (P351_IF70_SOQ_ZERO_MDBM - P351_IF70_TARGET_MDBM < 0 || \
     P351_IF70_SOQ_ZERO_MDBM - P351_IF70_TARGET_MDBM > 89750 || \
     (P351_IF70_SOQ_ZERO_MDBM - P351_IF70_TARGET_MDBM) % 250 != 0)
#error "SOQPSK measured calibration cannot reach -30dBm at a valid attenuation step"
#endif

static inline uint32_t p351_if70_power_calibrated(uint32_t mode)
{
    if (mode == 0U)
        return BOOT_PCM_FM_P351_POWER_CALIBRATED == 1U &&
            BOOT_PCM_FM_LO_HZ == P351_IF70_CENTER_HZ &&
            BOOT_PCM_FM_TARGET_MDBM == P351_IF70_TARGET_MDBM &&
            BOOT_PCM_FM_AMPLITUDE == P351_IF70_AMPLITUDE &&
            BOOT_PCM_FM_BW_HZ == P351_IF70_BANDWIDTH_HZ;
    if (mode == 1U) return P351_IF70_SOQ_POWER_CALIBRATED;
    return 0U; /* 旧CW=LO+2MHz；本固定70MHz协议不提供冒充70MHz的CW。 */
}

static inline uint32_t p351_if70_attenuation_mdb(uint32_t mode)
{
    if (!p351_if70_power_calibrated(mode)) return P351_IF70_SAFE_ATTEN_MDB;
    return mode == 0U ? (uint32_t)BOOT_PCM_FM_ATTEN_MDB :
        (uint32_t)(P351_IF70_SOQ_ZERO_MDBM - P351_IF70_TARGET_MDBM);
}

#endif
