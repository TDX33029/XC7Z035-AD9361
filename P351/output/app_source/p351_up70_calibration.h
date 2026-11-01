#ifndef P351_UP70_CALIBRATION_H
#define P351_UP70_CALIBRATION_H

#include <stdint.h>

/* 只做功率预算，不替代硬件资格、锁定检查或功率计。
 * mode: 0=PCM/FM, 1=SOQPSK；单位一律mdB/mdBm，避免浮点和符号混用。
 * confidence: 1=模型，2=逐模式/频段/码率实测表；measured不表示在线测量。
 */
typedef struct {
    uint32_t mode, rate_min_hz, rate_max_hz;
    int32_t zero_mdbm;
    uint32_t measured;
} p351_up70_if_record_t;
typedef struct {
    uint32_t mode, freq_min_khz, freq_max_khz, rate_min_hz, rate_max_hz;
    int32_t net_gain_mdb;
    uint32_t measured;
} p351_up70_rf_record_t;

#ifndef P351_UP70_CALIBRATION_DATA_HEADER
#define P351_UP70_CALIBRATION_DATA_HEADER "p351_up70_calibration_data.h"
#endif
#include P351_UP70_CALIBRATION_DATA_HEADER

typedef struct {
    uint32_t if_atten_mdb;
    int32_t predicted_if_mdbm;
    uint32_t confidence, measured;
} p351_up70_if_plan_t;
typedef struct {
    uint32_t if_atten_mdb;
    int32_t predicted_if_mdbm;
    uint32_t ext_half_db;
    int32_t predicted_final_mdbm;
    uint32_t confidence, measured;
} p351_up70_power_plan_t;

static inline int p351_up70_cal_frequency_valid(uint32_t khz)
{
    return khz % 100U == 0U && ((khz >= 5091000U && khz <= 5150000U) ||
        (khz >= 5925000U && khz <= 6125000U));
}

static inline int p351_up70_if_plan(uint32_t mode, uint32_t rate_hz,
                                   p351_up70_if_plan_t *out)
{
    uint32_t i, found=0U;
    int64_t need=0;
    p351_up70_if_plan_t p={0U,0,0U,0U};
    if (!out || mode>1U || rate_hz<100000U || rate_hz>20000000U) return 0;
    for(i=0U;i!=P351_UP70_IF_RECORD_COUNT;i++) {
        const p351_up70_if_record_t *r=&p351_up70_if_records[i];
        if(r->mode!=mode || rate_hz<r->rate_min_hz || rate_hz>r->rate_max_hz) continue;
        /* 重叠记录或手工非法头文件也拒绝，不选择任意一条。 */
        if(found++ || r->zero_mdbm < -100000 || r->zero_mdbm > 100000 || r->measured>1U) return 0;
        need=(int64_t)r->zero_mdbm+INT64_C(30000);
        if(need<0 || need>89750) return 0;
        /* 朝安全方向向上取0.25dB步进，使预测IF不高于-30dBm。 */
        p.if_atten_mdb=(uint32_t)((need+249)/250)*250U;
        if(p.if_atten_mdb>89750U) return 0;
        p.predicted_if_mdbm=r->zero_mdbm-(int32_t)p.if_atten_mdb;
        p.measured=r->measured;
        p.confidence=r->measured?2U:1U;
    }
    if(found!=1U) return 0;
    *out=p;
    return 1;
}

static inline int p351_up70_power_plan(uint32_t mode, uint32_t final_khz,
    uint32_t rate_hz, int32_t target_mdbm, p351_up70_power_plan_t *out)
{
    uint32_t i, found=0U;
    int64_t zero=0, need=0;
    p351_up70_if_plan_t ifp;
    p351_up70_power_plan_t p={0U,0,0U,0,0U,0U};
    if(!out || !p351_up70_cal_frequency_valid(final_khz) ||
       target_mdbm < -40000 || target_mdbm > -10000 || target_mdbm%500!=0 ||
       !p351_up70_if_plan(mode,rate_hz,&ifp)) return 0;
    for(i=0U;i!=P351_UP70_RF_RECORD_COUNT;i++) {
        const p351_up70_rf_record_t *r=&p351_up70_rf_records[i];
        if(r->mode!=mode || final_khz<r->freq_min_khz || final_khz>r->freq_max_khz ||
           rate_hz<r->rate_min_hz || rate_hz>r->rate_max_hz) continue;
        if(found++ || r->net_gain_mdb < -100000 || r->net_gain_mdb > 100000 || r->measured>1U) return 0;
        zero=(int64_t)ifp.predicted_if_mdbm+(int64_t)r->net_gain_mdb;
        need=zero-(int64_t)target_mdbm;
        if(need<0 || need>63000) return 0; /* TFS只允许0..126，127保留；不可达直接拒绝。 */
        p.ext_half_db=(uint32_t)((need+499)/500);
        if(p.ext_half_db>126U) return 0;
        p.if_atten_mdb=ifp.if_atten_mdb;
        p.predicted_if_mdbm=ifp.predicted_if_mdbm;
        p.predicted_final_mdbm=(int32_t)(zero-(int64_t)p.ext_half_db*500);
        p.measured=ifp.measured && r->measured;
        p.confidence=p.measured?2U:1U;
    }
    if(found!=1U) return 0;
    *out=p;
    return 1;
}
#endif
