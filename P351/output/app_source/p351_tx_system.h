#ifndef P351_TX_SYSTEM_H
#define P351_TX_SYSTEM_H
#include <stdint.h>

/* 历史双频段整机候选（屏幕v4/op11）。本轮需求已改为TRX2固定70MHz、目标-30dBm；
 * 本文件的5GHz直出/3725MHz和频规划不适用于新需求，不要用旧屏幕v4配置70MHz。
 * 资格宏仍保持0。70MHz的当前初始化参数见boot_pcm_fm_profile.h；外部变频由外设负责。 */

/* 整机频率/固定驱动策略。dB是相对量，不能从寄存器推出未经标定的dBm。
 * “总衰减0dB”定义：固定AD9361/数字驱动下，最终输出公共七线衰减器为0dB。
 * 面板只调公共衰减器0..63dB，不把固定片内20dB重复算入可调范围。
 * -10dBm是建议的TRX2驱动标定目标，不是本固件已经测得或保证的功率。
 * 各频点/调制模式均需实测后确定固定片内衰减；不得改CALIBRATED宏冒充标定。
 */
#define P351_DRIVER_TARGET_MDBM (-10000)
#ifndef P351_DRIVER_FIXED_ATTEN_MDB
#define P351_DRIVER_FIXED_ATTEN_MDB 20000U
#endif
#ifndef P351_DRIVER_POWER_CALIBRATED
#define P351_DRIVER_POWER_CALIBRATED 0U
#endif
#ifndef P351_FINAL_COMMON_ATTENUATOR_VERIFIED
#define P351_FINAL_COMMON_ATTENUATOR_VERIFIED 0U
#endif
/* 现有PL没有直出/上变频旁路控制。0=未确认，1=人工固定直出接线，2=人工固定上变频。
 * 此项不能用来实现自动切RF开关；新增开关后须另写驱动和失效安全回读。
 */
#ifndef P351_FINAL_MANUAL_PATH
#define P351_FINAL_MANUAL_PATH 0U
#endif
#ifndef P351_UPCONVERTER_VERIFIED
#define P351_UPCONVERTER_VERIFIED 0U
#endif
#define P351_FINAL_REQUIRE_EXTERNAL_10M 1U
#define P351_UPCONVERTER_LO_KHZ 3725000U
#define P351_UPCONVERTER_LOCK_STABLE_MS 20U
#define P351_SYSTEM_TIMEOUT_MS 5000U

#if P351_DRIVER_FIXED_ATTEN_MDB < 20000U || P351_DRIVER_FIXED_ATTEN_MDB > 89750U || P351_DRIVER_FIXED_ATTEN_MDB % 250U
#error "Fixed AD9361 driver attenuation must be 20..89.75dB in 0.25dB steps"
#endif
#if P351_DRIVER_POWER_CALIBRATED > 1U || P351_FINAL_COMMON_ATTENUATOR_VERIFIED > 1U || P351_UPCONVERTER_VERIFIED > 1U || P351_FINAL_MANUAL_PATH > 2U
#error "Invalid system RF qualification configuration"
#endif

enum { P351_PATH_UNKNOWN=0, P351_PATH_DIRECT=1, P351_PATH_UPCONVERT=2 };
typedef struct {
    uint32_t final_khz;     /* 用户要求的调制中心/CW主峰，不是TFS裸本振输出 */
    uint32_t signal_khz;    /* TRX2输出调制中心/CW主峰 */
    uint32_t ad_lo_khz;     /* AD9361 LO；CW会减去现有PL DDS的+2MHz */
    uint32_t tfs_lo_khz;    /* 只在已确认和频上变频器存在时写给本振源 */
    uint32_t path;
} p351_tx_plan_t;

/* 低侧LO/和频是候选频率规划：5925..6125 - 3725 = 2200..2400MHz。
 * TFS21-22公开手册只有REF in/RF out，没有IF输入，因此它本身不等于混频组件。
 * 本函数只做数学规划，不允许用它的成功返回代替实际混频器/滤波/旁路验收。
 */
static inline int p351_tx_plan(uint32_t mode, uint32_t final_khz, p351_tx_plan_t *out)
{
    p351_tx_plan_t p;
    if(!out || mode>2U) return 0;
    p.final_khz=final_khz;
    if(final_khz>=5091000U && final_khz<=5150000U) {
        p.path=P351_PATH_DIRECT; p.signal_khz=final_khz; p.tfs_lo_khz=0;
    } else if(final_khz>=5925000U && final_khz<=6125000U) {
        p.path=P351_PATH_UPCONVERT;
        p.signal_khz=final_khz-P351_UPCONVERTER_LO_KHZ;
        p.tfs_lo_khz=P351_UPCONVERTER_LO_KHZ;
    } else return 0;
    p.ad_lo_khz=p.signal_khz-(mode==2U?2000U:0U);
    *out=p;
    return 1;
}

/* 位0未标定驱动，位1公共衰减器未核验，位2物理路径不匹配/未确认，
 * 位3上变频器未核验。默认返回非零，禁止把候选算法当可发射的整机。
 * 用户确认“希望有这些功能”不等于实际存在IF混频/旁路/滤波硬件。
 */
static inline uint32_t p351_tx_plan_blockers(const p351_tx_plan_t *p)
{
    if(!p) return 15U;
    return (!P351_DRIVER_POWER_CALIBRATED?1U:0U) |
           (!P351_FINAL_COMMON_ATTENUATOR_VERIFIED?2U:0U) |
           ((P351_FINAL_MANUAL_PATH!=p->path)?4U:0U) |
           ((p->path==P351_PATH_UPCONVERT && !P351_UPCONVERTER_VERIFIED)?8U:0U);
}
#endif
