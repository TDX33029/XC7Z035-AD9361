#ifndef SCREEN_PROTOCOL_H_
#define SCREEN_PROTOCOL_H_

#include <stdint.h>
#include <string.h>

/* 自定义屏->PS事务，不是TJC固有指令：A5 5A 01 01 + 11个小端u32 + FF FF FF。
 * 前10项依次为序号、调制、LO/MHz、衰减/dB、码率/Mbit/s、PCM码型、帧开关、
 * 小帧字节数、复帧小帧数、无限循环开关；第11项是前10项加上0x105A5的和。
 * 定长51字节，数值中的FF不得当结束符。加和校验仅用于短TTL链路差错检测，非CRC。
 * STOP为A5 5A 01 02 FF FF FF，不依赖页面/握手/参数有效性。
 */
#define SCREEN_APPLY_BYTES 51U
#define SCREEN_CHECK_SEED UINT32_C(0x105A5)
#define SCREEN_START_TIMEOUT_MS 500U
#define SCREEN_MOD_AMPLITUDE 26214U
#define SCREEN_LEGACY_TOUCH 0 /* 旧ID4/ID5与新页面冲突：不得用原触摸ID开启RF。 */

typedef struct {
    uint32_t sequence, mode, lo_mhz, attenuation_db, rate_mbps;
    uint32_t line_code, framed, frame_bytes, frame_count, infinite;
    /* 仅PS内部扩展；不改变既有v1/v2线报文或旧加和范围。
     * sync_bits=0且word=0：沿用默认FE6B2840/32；否则长度16..33。
     * 第32位在sync_word_high最低位，使用64位拼接避免33位截断。 */
    uint32_t sync_word_low, sync_word_high, sync_bits;
    /* system_profile=1由整机RF事务调用：外衰减先63dB保护换频/校准，
     * 目标total_atten_halfdb(0..126)在最后开RF前设置。均不进入旧线报文。 */
    uint32_t system_profile, total_atten_halfdb;
    /* PS内部模拟带宽：0沿用旧接口20MHz；新固定70MHz事务明确使用56MHz。
     * 此字段不进入任何UART线报文，旧解码器初始化为0，不重解释旧参数。 */
    uint32_t rf_bandwidth_hz;
} screen_apply_t;

static inline uint32_t screen_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int screen_apply_valid(const screen_apply_t *c)
{
    if (!c || c->sequence == 0U || c->sequence > 65535U || c->mode > 2U ||
        c->lo_mhz < 70U || c->lo_mhz > 6000U || c->attenuation_db > 89U ||
        c->line_code > 6U || c->framed > 1U || c->infinite > 1U ||
        c->rate_mbps > 20U || c->frame_bytes < 5U || c->frame_bytes > 2048U ||
        c->frame_count == 0U || c->frame_count > 256U ||
        c->system_profile > 1U || c->total_atten_halfdb > 126U ||
        (!c->system_profile && c->total_atten_halfdb))
        return 0;
    if (c->sync_bits) {
        uint64_t word=(uint64_t)c->sync_word_low | ((uint64_t)c->sync_word_high << 32);
        if (c->sync_bits < 16U || c->sync_bits > 33U || (word >> c->sync_bits))
            return 0;
        if (c->mode != 2U && c->framed &&
            c->frame_bytes * 8U <= c->sync_bits + (c->frame_count > 1U ? 8U : 0U))
            return 0;
    } else if (c->sync_word_low || c->sync_word_high) return 0;
    /* 连CW/无帧模式的备用字段也限定范围；现有屏端固定发送512/1等合法默认。
     * 保证全部字段/校验和最高字节为0，独立STOP串不可能藏在合法APPLY里。 */
    /* 本屏采用整数Mbit/s控件；PS后台仍支持100 kbit/s，未伪装成屏上可设置。 */
    if (c->mode != 2U && (c->rate_mbps < 1U || c->rate_mbps > 20U))
        return 0;
    /* 连续积分相位核已覆盖全部七种PCM码型到20M；不沿用旧10M限制。
     * 双相码型的RF带宽及接收端兼容性仍需单独验收。 */
    if (c->mode == 2U && c->attenuation_db < 20U)
        return 0; /* 屏幕CW诊断保守限功率，不把自动限幅当作按请求执行。 */
    if (c->mode != 2U && c->framed &&
        (c->frame_bytes < 5U || c->frame_bytes > 2048U ||
         c->frame_count == 0U || c->frame_count > 256U ||
         (c->frame_count > 1U && c->frame_bytes < 6U) ||
         c->frame_bytes * c->frame_count > 16384U))
        return 0;
    return 1;
}

static inline int screen_apply_decode(const uint8_t *p, uint32_t n, screen_apply_t *c)
{
    uint32_t w[11], i, sum = SCREEN_CHECK_SEED;
    screen_apply_t candidate={0};
    if (!p || !c || n != SCREEN_APPLY_BYTES || p[0] != 0xA5U ||
        p[1] != 0x5AU || p[2] != 1U || p[3] != 1U ||
        p[48] != 255U || p[49] != 255U || p[50] != 255U)
        return 0;
    for (i = 0U; i < 11U; ++i) {
        w[i] = screen_u32(p + 4U + 4U*i);
        if (i < 10U) sum += w[i];
    }
    if (sum != w[10]) return 0;
    candidate.sequence=w[0]; candidate.mode=w[1]; candidate.lo_mhz=w[2];
    candidate.attenuation_db=w[3]; candidate.rate_mbps=w[4]; candidate.line_code=w[5];
    candidate.framed=w[6]; candidate.frame_bytes=w[7]; candidate.frame_count=w[8];
    candidate.infinite=w[9];
    if (!screen_apply_valid(&candidate)) return 0;
    *c = candidate; /* 全部校验后一次提交，坏包不得改动原请求。 */
    return 1;
}
#endif
