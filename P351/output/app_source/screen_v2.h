#ifndef P351_SCREEN_V2_H
#define P351_SCREEN_V2_H
#include "screen_protocol.h"
#include "p351_upconverter70.h"

/* v2共51字节：A5 5A 02 OP，10个LE32（seq+9参数），LE32 CRC16，FF FF FF。
 * v3共63字节：A5 5A 03 0B，13个LE32（seq+12参数），LE32 CRC16，FF FF FF。
 * v4共63字节：A5 5A 04 0C，同样13个LE32，参数仅调制/PN11组帧。
 * v5共63字节：A5 5A 05 0D，在v4前10项后加入最终RF kHz和负功率毫dBm绝对值。
 * v5是整机请求：未完成双本振接线及后级功率标定前回NOT_READY，不降级成IF发射。
 * v4固定IF、只读QUERY与独立STOP保持；其他旧写命令回NOT_READY。
 * CRC16/MODBUS分别覆盖前44/56字节；CRC字高16位必须为0。不是TJC系统协议。
 * 所有输入字最高字节为0，保证合法事务内部不可能包含独立STOP序列。
 * 保留v1、急停兼容；v2旧TX的衰减仍为0.25dB，绝不静默改单位。
 * v3/RF_TX：最终射频kHz、kbit/s、外部总衰减0.5dB；同步字拆分为
 * arg9低16、arg10中16、arg11低8位长度16..33及bit8同步字第32位。 */
enum { SC_TX=1, SC_QUERY=3, SC_REF=4, SC_TFS=5, SC_ATT=6,
       SC_TFS_READ=7, SC_RX=8, SC_RX_STOP=10, SC_RF_TX=11, SC_IF70_TX=12,
       SC_UP70_TX=13, SC_UP70_PRECISE_TX=14 };
enum { SC_OK=0, SC_PARAM=1, SC_BUSY=2, SC_FAILED=3, SC_PENDING=4,
       SC_TIMEOUT=5, SC_STOPPED=6, SC_SEQUENCE=7, SC_NOT_READY=8 };
typedef struct { uint32_t sequence, op, arg[12]; } screen_command_t;

/* v6/op14仍为63字节。arg1=整数kbit/s，arg9[19:10]=余数0..999bit/s；
 * [8:0]保持同步字宽度/第33位，[9]及其余位保留0。所有字最高字节仍为0，
 * 保持独立STOP扫描的无碰撞条件。旧v5/op13的单位绝不静默改变。 */
static inline uint32_t screen_command_rate_hz(const screen_command_t *c)
{
    return c->arg[1]*1000U +
        (c->op==SC_UP70_PRECISE_TX ? ((c->arg[9]>>10)&1023U) : 0U);
}

static inline uint16_t screen_crc16(const uint8_t *p, uint32_t n)
{
    uint16_t crc=0xffffU;
    uint32_t i;
    while (n--) {
        crc ^= *p++;
        for (i=0;i<8U;++i) crc=(uint16_t)((crc>>1)^((crc&1U)?0xa001U:0U));
    }
    return crc;
}
static inline int screen_command_valid(const screen_command_t *c)
{
    uint32_t i, used=0;
    const uint32_t *a;
    if (!c || !c->sequence || c->sequence>65535U) return 0;
    a=c->arg;
    for (i=0;i<12U;++i) if (a[i]>0xffffffU) return 0;
    switch(c->op) {
    case SC_IF70_TX: case SC_UP70_TX: case SC_UP70_PRECISE_TX: {
        /* v4/op12只传调制/组帧草稿，不接收LO、功率、参考源或上变频设置。
         * 参数为mode,rate/kbit/s,line,framed,bytes,count,infinite,
         * sync低16,中16,bits|(第32位<<8),保留0,保留0。 */
        uint32_t bits=a[9]&255U;
        uint64_t sync=(uint64_t)a[7] | ((uint64_t)a[8]<<16) |
                      ((uint64_t)((a[9]>>8)&1U)<<32);
        uint32_t precise=c->op==SC_UP70_PRECISE_TX;
        used=c->op==SC_IF70_TX ? 10U : 12U;
        if(c->op!=SC_IF70_TX &&
           (!p351_up70_frequency_valid(a[10]) || !p351_up70_power_valid(a[11]))) return 0;
        if(a[0]>1U || a[1]<100U || a[1]>20000U || a[2]>6U ||
           (a[0]==1U && a[2]) || a[3]>1U || a[4]<1U || a[4]>2048U ||
           !a[5] || a[5]>256U || a[4]*a[5]>16384U || a[6]>1U ||
           a[7]>65535U || a[8]>65535U ||
           (precise ? ((a[9]&~0x000FFDFFU) || ((a[9]>>10)&1023U)>999U) : a[9]>511U) ||
           screen_command_rate_hz(c)>20000000U ||
           bits<16U || bits>33U || (sync>>bits) ||
           (a[3] && a[4]*8U<=bits+(a[5]>1U?8U:0U))) return 0;
        break;
    }
    case SC_TX:
        used=9;
        if (a[0]>2 || a[1]<70000 || a[1]>6000000 || a[2]>359 ||
            a[3]<100 || a[3]>20000 || a[4]>6 || a[5]>1 || a[8]>1 ||
            a[6]<5 || a[6]>2048 || !a[7] || a[7]>256 ||
            a[6]*a[7]>16384 || (a[7]>1 && a[6]<6) ||
            (a[0]==2 && a[2]<80) || (a[0]!=0 && a[4])) return 0;
        break;
    case SC_RF_TX: {
        uint32_t bits=a[11]&255U;
        uint64_t sync=(uint64_t)a[9] | ((uint64_t)a[10]<<16) |
                      ((uint64_t)((a[11]>>8)&1U)<<32);
        used=12;
        if (a[0]>2 || !((a[1]>=5091000U && a[1]<=5150000U) ||
                          (a[1]>=5925000U && a[1]<=6125000U)) ||
            a[2]>126 || a[3]<100 || a[3]>20000 || a[4]>6 ||
            a[5]>1 || a[8]>1 || a[6]<1 || a[6]>2048 ||
            !a[7] || a[7]>256 || a[6]*a[7]>16384 ||
            (a[0]!=0 && a[4]) || a[9]>65535 || a[10]>65535 ||
            a[11]>511 || bits<16 || bits>33 || (sync>>bits) ||
            (a[5] && a[6]*8U<=bits+(a[7]>1U?8U:0U))) return 0;
        break;
    }
    case SC_REF: used=1; if(a[0]>1) return 0; break;
    case SC_TFS:
        used=2;
        if(a[0]>65535 || a[1]>65535 ||
           ((uint64_t)a[1]*65536U+a[0])<50000U ||
           ((uint64_t)a[1]*65536U+a[0])>22600000U) return 0;
        break;
    case SC_ATT: used=1; if(a[0]>126) return 0; break;
    case SC_TFS_READ: used=1; if(a[0]>3) return 0; break;
    case SC_RX:
        used=8;
        if(a[0]>1 || a[1]<70000 || a[1]>6000000 ||
           a[2]<100 || a[2]>20000 || a[3]<200 || a[3]>56000 ||
           a[3]<a[2] || a[4]>60 || a[5]>6 || (a[0]==1 && a[5]) ||
           a[6]<8 || a[6]>2048 || (a[6]&3U) || !a[7] || a[7]>256 ||
           a[6]*a[7]>16384) return 0;
        break;
    case SC_QUERY: case SC_RX_STOP: break;
    default: return 0;
    }
    for(i=used;i<12U;++i) if(a[i]) return 0;
    return 1;
}
static inline int screen_command_decode(const uint8_t *p, uint32_t n, screen_command_t *out)
{
    screen_command_t c={0};
    uint32_t i,words,crc_at;
    if(!p || !out || n<4 || p[0]!=0xa5 || p[1]!=0x5a) return 0;
    if(p[2]==2 && n==51 && p[3]!=SC_RF_TX && p[3]!=SC_IF70_TX && p[3]!=SC_UP70_TX && p[3]!=SC_UP70_PRECISE_TX) { words=9;crc_at=44; }
    else if(p[2]==3 && n==63 && p[3]==SC_RF_TX) { words=12;crc_at=56; }
    else if(p[2]==4 && n==63 && p[3]==SC_IF70_TX) { words=12;crc_at=56; }
    else if(p[2]==5 && n==63 && p[3]==SC_UP70_TX) { words=12;crc_at=56; }
    else if(p[2]==6 && n==63 && p[3]==SC_UP70_PRECISE_TX) { words=12;crc_at=56; }
    else return 0;
    if(p[n-3]!=255 || p[n-2]!=255 || p[n-1]!=255 ||
       screen_u32(p+crc_at)!=screen_crc16(p,crc_at)) return 0;
    c.sequence=screen_u32(p+4); c.op=p[3];
    for(i=0;i<words;++i) c.arg[i]=screen_u32(p+8+4*i);
    if(!screen_command_valid(&c)) return 0;
    *out=c;
    return 1;
}
#endif
