#ifndef BOOT_PCM_FM_PROFILE_H
#define BOOT_PCM_FM_PROFILE_H

#include <stdint.h>
#include "pn11_frame.h"

/* 上电参数唯一入口（UTF-8）：修改下列常量、保存，再重新编译/固化。
 * 是否自动发射由 ENABLED 决定；--boot-pcm-fm 仅在隔离构建副本中强制开启。
 * 当前自动发射仅支持 PCM/FM、物理TX2；默认外10MHz驯服40MHz见p351_clock_profile.h。
 * 当前需求：TRX2输出70MHz调制中心、目标-30dBm，其余交外部上变频器。
 * 本任务不控制外部最终频率；不得套用旧的5GHz/6GHz双段频率规划。
 * 物理端口选择见rf_channel_profile.h；这里的模式/衰减仍使用唯一逻辑槽0。
 * 数值请使用十进制/0x十六进制常量，不要在用户参数中填写表达式。
 */
#define BOOT_PCM_FM_ENABLED 0
/* P351带板载功放：旧Z7的20dBm基准不是新板实测功率。
 * 只有完成TRX2频点/幅度/调制标定并更新下方基准后，才能将此项改为1。
 * 禁止只开启ENABLED就沿用旧板功率估算。普通UI使用相对衰减，不依赖此值。 */
#define BOOT_PCM_FM_P351_POWER_CALIBRATED 0
#if BOOT_PCM_FM_ENABLED && !BOOT_PCM_FM_P351_POWER_CALIBRATED
#error "P351 automatic TX requires measured TRX2 power calibration"
#endif

#define BOOT_PCM_FM_LO_HZ       UINT64_C(70000000) /* Hz：本轮固定中频目标70MHz；PCM/FM调制中心 */
#define BOOT_PCM_FM_RATE_HZ     4000000U /* bit/s：100 k～20 M；七码型数字链路均支持20 M，RF须实测 */
#define BOOT_PCM_FM_BW_HZ       56000000U /* 固定56MHz模拟带宽覆盖高码率；不是B99，实测标定也须使用此值 */
#define BOOT_PCM_FM_AMPLITUDE   26214U /* 数字幅度：1～32767；26214约为0.80 */
#define BOOT_PCM_FM_LINE_CODE   0U /* 0 NRZ-L，1 NRZ-M，2 NRZ-S，3/4/5双相L/M/S，6 RNRZ-L */
#define BOOT_PCM_FM_RANDOMIZER  0U /* 0关闭、1 IRIG15；码型6必须为1，其他码型必须为0 */
#define BOOT_PCM_FM_ZERO_MDBM   0 /* 未标定占位值，绝非TRX2真实0衰减功率；CALIBRATED=1前必须填写标定记录 */
#define BOOT_PCM_FM_TARGET_MDBM -30000 /* TRX2目标-30dBm，不等于AD9361衰减30dB */
#if BOOT_PCM_FM_P351_POWER_CALIBRATED
#define BOOT_PCM_FM_ATTEN_MDB   (BOOT_PCM_FM_ZERO_MDBM - BOOT_PCM_FM_TARGET_MDBM)
#else
#define BOOT_PCM_FM_ATTEN_MDB   89750 /* 未标定时只保留最大衰减保护，不声称已经输出-30dBm */
#endif
#define BOOT_PCM_FM_TASK_ID     0x50434D01U /* 任务ID，通常不必修改 */
#define BOOT_PCM_FM_REPEAT      UINT32_C(0xFFFFFFFF) /* 附加重播次数：0单次，0xFFFFFFFF无限 */
#define BOOT_PCM_FM_START_TIMEOUT_MS 500U /* PLL/PL各阶段超时，ms：100～2000 */

/* 数据与组帧：仅PN11。同步字/帧头不参与PN状态推进；整个缓存重播时PN重新起始。 */
#define BOOT_PCM_FM_PN_SEED     0x7FFU /* PN11初值：1～0x7FF，禁止全0 */
#define BOOT_PCM_FM_PN_INVERT   0U /* 0正常、1反相；只反相PN载荷 */
#define BOOT_PCM_FM_FRAMED      1U /* 1加入帧同步，0发送无帧PN11 */
#define BOOT_PCM_FM_RAW_BITS    2047U /* 无帧时有效bit数：1～131072；2047为PN11自然周期 */
#define BOOT_PCM_FM_SYNC_WORD   UINT64_C(0xFE6B2840) /* MSB先发，与SYNC_BITS匹配 */
#define BOOT_PCM_FM_SYNC_BITS   32U /* 同步字长度：16～33 bit */
#define BOOT_PCM_FM_FRAME_BYTES 512U /* 小帧总字节数：1～2048，必须留出载荷 */
#define BOOT_PCM_FM_FRAME_COUNT 1U /* 1只有小帧；2～256自动插入8bit SFID复帧 */
#define BOOT_PCM_FM_SFID_START  0U /* SFID从0或1开始；起值+小帧数不可超过256 */

/* 编译前拒绝非法配置；Flash构建也检查相同边界，防止静默截断或回退。 */
#if BOOT_PCM_FM_ENABLED != 0 && BOOT_PCM_FM_ENABLED != 1
#error "BOOT_PCM_FM_ENABLED must be 0 or 1"
#endif
#if BOOT_PCM_FM_LO_HZ < UINT64_C(70000000) || BOOT_PCM_FM_LO_HZ > UINT64_C(6000000000)
#error "Boot TX LO outside 70 MHz..6 GHz"
#endif
#if BOOT_PCM_FM_RATE_HZ < 100000U || BOOT_PCM_FM_RATE_HZ > 20000000U
#error "Boot bit rate outside 100 k..20 M bit/s"
#endif
#if BOOT_PCM_FM_BW_HZ < 200000U || BOOT_PCM_FM_BW_HZ > 56000000U || BOOT_PCM_FM_AMPLITUDE < 1U || BOOT_PCM_FM_AMPLITUDE > 32767U
#error "Boot bandwidth or amplitude out of range"
#endif
#if BOOT_PCM_FM_LINE_CODE < 0 || BOOT_PCM_FM_LINE_CODE > 6U || BOOT_PCM_FM_RANDOMIZER != (BOOT_PCM_FM_LINE_CODE == 6U)
#error "RNRZ-L requires IRIG15; other line codes require randomizer OFF"
#endif
#if BOOT_PCM_FM_ATTEN_MDB < 0 || BOOT_PCM_FM_ATTEN_MDB > 89750 || BOOT_PCM_FM_ATTEN_MDB % 250 != 0
#error "Boot attenuation must be 0..89750 mdB in steps of 250 mdB"
#endif
#if BOOT_PCM_FM_ZERO_MDBM < -100000 || BOOT_PCM_FM_ZERO_MDBM > 100000 || BOOT_PCM_FM_TARGET_MDBM < -100000 || BOOT_PCM_FM_TARGET_MDBM > 100000
#error "Boot power calibration outside -100..100 dBm"
#endif
#if BOOT_PCM_FM_TASK_ID < 0 || BOOT_PCM_FM_TASK_ID > UINT32_C(0xFFFFFFFF) || BOOT_PCM_FM_REPEAT < 0 || BOOT_PCM_FM_REPEAT > UINT32_C(0xFFFFFFFF)
#error "Task/repeat must fit unsigned 32 bits"
#endif
#if BOOT_PCM_FM_START_TIMEOUT_MS < 100U || BOOT_PCM_FM_START_TIMEOUT_MS > 2000U
#error "Boot stage timeout outside 100..2000 ms"
#endif
#if BOOT_PCM_FM_PN_SEED == 0 || BOOT_PCM_FM_PN_SEED > 0x7FFU || BOOT_PCM_FM_PN_INVERT > 1U || BOOT_PCM_FM_FRAMED > 1U
#error "Invalid boot PN11 source"
#endif
#if BOOT_PCM_FM_RAW_BITS == 0 || BOOT_PCM_FM_RAW_BITS > 131072U
#error "Invalid unframed boot buffer length"
#endif
#if BOOT_PCM_FM_SYNC_BITS < 16U || BOOT_PCM_FM_SYNC_BITS > 33U || BOOT_PCM_FM_SYNC_WORD >= (UINT64_C(1) << BOOT_PCM_FM_SYNC_BITS)
#error "Invalid boot synchronization word/length"
#endif
#if BOOT_PCM_FM_FRAME_BYTES == 0 || BOOT_PCM_FM_FRAME_BYTES > 2048U || BOOT_PCM_FM_FRAME_COUNT == 0 || BOOT_PCM_FM_FRAME_COUNT > 256U || BOOT_PCM_FM_SFID_START > 1U
#error "Invalid boot minor/major frame parameters"
#endif
#if BOOT_PCM_FM_FRAME_COUNT > 1U && BOOT_PCM_FM_SFID_START + BOOT_PCM_FM_FRAME_COUNT > 256U
#error "SFID would overflow 8 bits"
#endif
#if BOOT_PCM_FM_FRAME_BYTES * BOOT_PCM_FM_FRAME_COUNT > 16384U || BOOT_PCM_FM_FRAME_BYTES * 8U <= BOOT_PCM_FM_SYNC_BITS + (BOOT_PCM_FM_FRAME_COUNT > 1U ? 8U : 0U)
#error "Boot frames exceed replay capacity or contain no payload"
#endif

/* 人工PC直发与屏幕整机的初始化默认值分开。二者均不自动发射。
 * PC人工版待机LO=2.4GHz，后续TXCF可设70MHz～6GHz；屏幕仍按70MHz IF流程。
 * 改LO不能绕过停发/QEC/锁定校验；此值不表示CW峰（CW峰=LO+2MHz）。 */
#if P351_MANUAL_RF_TEST
#define BOOT_INITIAL_TX_LO_HZ UINT64_C(2400000000)
#else
#define BOOT_INITIAL_TX_LO_HZ BOOT_PCM_FM_LO_HZ
#endif

/* 内部40 MHz参考经RFPLL分数分频、整数Hz回算后，读回可能相差几个Hz。
 * 此固定容差仅覆盖数字量化，不是晶振精度/PLL锁定容差，也不允许错频启动。
 * 不用32位差值或有符号减法，避免5 GHz以上频率及非法读数溢出。 */
#define BOOT_PCM_FM_LO_READBACK_TOLERANCE_HZ 10U

static inline int boot_pcm_fm_lo_readback_valid(uint64_t readback_hz)
{
    const uint64_t error_hz = readback_hz >= BOOT_PCM_FM_LO_HZ ?
        readback_hz - BOOT_PCM_FM_LO_HZ : BOOT_PCM_FM_LO_HZ - readback_hz;
    return readback_hz != 0U && error_hz <= BOOT_PCM_FM_LO_READBACK_TOLERANCE_HZ;
}

static inline uint32_t boot_pcm_fm_rate_word(uint32_t sample_hz)
{
    return (uint32_t)((((uint64_t)BOOT_PCM_FM_RATE_HZ << 32) + sample_hz / 2U) /
                      sample_hz);
}

static inline uint32_t boot_pcm_fm_deviation_word(uint32_t rate_word)
{
    return (uint32_t)(((uint64_t)rate_word * 35U + 50U) / 100U);
}

/* 调频峰值频偏保持0.35Rb，四阶Bessel保持0.7Rb，不允许修改成型阶数。 */
static inline void boot_pcm_fm_frame_config(pn11_frame_config_t *cfg)
{
    pn11_frame_defaults(cfg);
    cfg->seed = BOOT_PCM_FM_PN_SEED;
    cfg->invert = BOOT_PCM_FM_PN_INVERT;
    cfg->framed = BOOT_PCM_FM_FRAMED;
    cfg->raw_bits = BOOT_PCM_FM_RAW_BITS;
    cfg->sync_word = BOOT_PCM_FM_SYNC_WORD;
    cfg->sync_bits = BOOT_PCM_FM_SYNC_BITS;
    cfg->frame_bytes = BOOT_PCM_FM_FRAME_BYTES;
    cfg->frame_count = BOOT_PCM_FM_FRAME_COUNT;
    cfg->sfid_start = BOOT_PCM_FM_SFID_START;
}

#endif
