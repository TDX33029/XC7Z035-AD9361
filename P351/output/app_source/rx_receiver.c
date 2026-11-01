/* 接收控制状态与实时位流服务。每次改频率/码率都停止旧解调、完整校验后
 * 重启新epoch；不把不同参数或缺样的数据静默拼接。原TX参数完全独立。 */
#include "rx_receiver.h"
#include "rx_receiver_profile.h"
#include "dma_rx.h"
#include "p351_board.h"
#include "xil_io.h"
#include "sleep.h"
#include <string.h>
static const rx_receiver_config_t defaults={RX_RECEIVER_DEFAULT_LO_HZ,RX_RECEIVER_DEFAULT_RATE_HZ,
    RX_RECEIVER_DEFAULT_BW_HZ,RX_RECEIVER_DEFAULT_GAIN_DB,RX_RECEIVER_DEFAULT_SYNC_WORD,
    RX_RECEIVER_DEFAULT_FRAME_BYTES,RX_RECEIVER_DEFAULT_FRAME_COUNT,0,RX_RECEIVER_DEFAULT_MODE,RX_RECEIVER_DEFAULT_LINE_CODE,
    RX_RECEIVER_DEFAULT_SYNC_BITS,RX_RECEIVER_DEFAULT_SFID_START};
static rx_receiver_config_t current={RX_RECEIVER_DEFAULT_LO_HZ,RX_RECEIVER_DEFAULT_RATE_HZ,
    RX_RECEIVER_DEFAULT_BW_HZ,RX_RECEIVER_DEFAULT_GAIN_DB,RX_RECEIVER_DEFAULT_SYNC_WORD,
    RX_RECEIVER_DEFAULT_FRAME_BYTES,RX_RECEIVER_DEFAULT_FRAME_COUNT,0,RX_RECEIVER_DEFAULT_MODE,RX_RECEIVER_DEFAULT_LINE_CODE,
    RX_RECEIVER_DEFAULT_SYNC_BITS,RX_RECEIVER_DEFAULT_SFID_START};
static u32 epoch,sequence,bit_offset,network_drops,last_error;
static u8 stop_pending;
static u64 totals[4];
static u32 previous[4];
static u32 read_reg(u32 offset){return Xil_In32(RX2R_PACK_BASE+offset);}
static int write_reg(u32 offset,u32 value){
    Xil_Out32(RX2R_PACK_BASE+offset,value);
    return read_reg(offset)==value;
}
static u32 counter(u32 select){
    Xil_Out32(RX2R_PACK_BASE+RXR_SELECT,select);
    return read_reg(RXR_COUNTER);
}
static u32 rate_word_for(u32 rate){return (u32)((((u64)rate<<32)+30720000ULL)/61440000ULL);}
static int stage_config(const rx_receiver_config_t *cfg){
    u32 rate_word=rate_word_for(cfg->bit_rate);
    return write_reg(RXR_RATE,rate_word) &&
        write_reg(RXR_SELECT,0x80000000U|(u32)((1ULL<<48)/((u64)rate_word*4U))) &&
        write_reg(RXR_SYNC,(u32)cfg->sync_word) && write_reg(RXR_FRAME,(u32)cfg->frame_bytes*8U |
            ((u32)cfg->frame_count<<16) | ((u32)cfg->sfid_start<<25) | ((u32)cfg->sync_bits<<26));
}
/* 停止尚未确认时继续占用接收资源，防止TX/监视事务误判为空闲。 */
u8 rx_receiver_active(void){return current.enabled || stop_pending;}
u32 rx_receiver_init(void){
    u32 result;
    /* PS重跑ELF不一定复位PL；不能让上次解调继续运行却报告“已停止”。 */
    if(!p351_rx_dsp_capable() || !p351_rx_timing_capable() || counter(7)!=RXR_ABI){last_error=2;return 2;}
    result=rx_receiver_stop();
    if(result){last_error=result;return result;} /* 保留未确认停止的状态及最后统计。 */
    current=defaults;
    if(!stage_config(&defaults))result=5;
    epoch=sequence=bit_offset=network_drops=0;memset(totals,0,sizeof totals);memset(previous,0,sizeof previous);
    last_error=result;return result;
}
/* 20 Mbit/s的32位计数约215秒回绕一次；主循环逐次累加模2^32差值。
 * 运行期间不能阻塞主循环超过一次回绕时间。停止前取最后一组统计，
 * 停止后只读缓存，不能把PL复位到0误算成又发生一次回绕。 */
static void update_totals(void){
    u32 i,value;
    if(!current.enabled || stop_pending)return;
    for(i=4;i>0;i--){
        u32 index=i-1; /* 先误码、后载荷，避免跨读取时刻出现误码数大于已读载荷数。 */
        value=counter(index);totals[index]+=(u32)(value-previous[index]);previous[index]=value;
    }
}
u32 rx_receiver_stop(void){
    u32 retry;
    if(!p351_rx_dsp_capable()){last_error=2;return 2;}
    update_totals();stop_pending=1;
    if(!write_reg(RXR_CTRL,0)){last_error=5;return 5;}
    for(retry=0;retry<1000;retry++){
        if(!(read_reg(RXR_STATUS)&1U)){
            current.enabled=0;stop_pending=0;last_error=0;return 0;
        }
        usleep(1);
    }
    last_error=6;return 6;
}
u32 rx_receiver_apply(const rx_receiver_config_t *cfg){
    u32 result,control,retry;
    if(!cfg || cfg->enabled>1U)return 1;
    if(!cfg->enabled){last_error=rx_receiver_stop();return last_error;}
    if(cfg->mode>1U || cfg->line_code>6U ||
        (cfg->mode==1U && cfg->line_code!=0U) || cfg->center_hz<70000000ULL ||
        cfg->center_hz>6000000000ULL || cfg->bit_rate<100000U || cfg->bit_rate>20000000U ||
        cfg->bandwidth_hz<200000U || cfg->bandwidth_hz>56000000U || cfg->bandwidth_hz<cfg->bit_rate ||
        cfg->gain_db<0 || cfg->gain_db>60 || cfg->frame_bytes<8U || cfg->frame_bytes>2048U || (cfg->frame_bytes&3U) ||
        cfg->frame_count<1U || cfg->frame_count>256U || cfg->sfid_start>1U ||
        (cfg->frame_count>1U && cfg->frame_count+cfg->sfid_start>256U) ||
        (u32)cfg->frame_bytes*cfg->frame_count>16384U || cfg->sync_bits<16U || cfg->sync_bits>33U ||
        (cfg->sync_word>>cfg->sync_bits))return 1;
    if(!p351_rx_dsp_capable() || !p351_rx_timing_capable() || counter(7)!=RXR_ABI)return 2;
    result=board_receiver_can_apply(cfg);if(result)return result; /* 非法/忙不得停止旧任务。 */
    result=rx_receiver_stop();if(result){last_error=result;return result;}
    result=board_receiver_tune(cfg);if(result){last_error=result;return result;}
    control=1U | (cfg->mode?2U:0U) | ((cfg->mode || cfg->line_code==6U)?4U:0U) |
        ((u32)cfg->line_code<<3) | ((u32)(cfg->sync_word>>32)<<6);
    if(!stage_config(cfg) || !write_reg(RXR_CTRL,control))goto failed;
    for(retry=0;retry<1000;retry++){
        if(read_reg(RXR_STATUS)&1U)break;
        usleep(1);
    }
    if(retry==1000)goto failed;
    memset(previous,0,sizeof previous);memset(totals,0,sizeof totals);
    current=*cfg;epoch++;sequence=bit_offset=network_drops=0;last_error=0;
    return 0;
failed:
    {u32 stopped=rx_receiver_stop();last_error=stopped?stopped:5U;return last_error;}
}
void rx_receiver_service(void){
    u32 packet[128],status,n,k,total=0;
    if(!current.enabled || stop_pending)return;
    update_totals();status=read_reg(RXR_STATUS);
    if(status&12U){last_error=(status&8U)?8U:7U;return;} /* 保留故障统计，等待显式停止/重配。 */
    while(total<2048U){
        status=read_reg(RXR_STATUS);n=(status>>15)&2047U;
        if(!n)break;
        if(n>128)n=128;
        for(k=0;k<n;k++)packet[k]=read_reg(RXR_DATA);
        if(udp_send_receiver_bits(packet,(u16)n,epoch,sequence,bit_offset,current.bit_rate,status)!=0)network_drops++;
        sequence++;bit_offset+=n*32U;total+=n;
    }
}
void rx_receiver_snapshot(u32 w[RXR_CFG_WORDS]){
    memset(w,0,RXR_CFG_WORDS*sizeof(*w));
    w[0]=3;w[1]=rx_receiver_active();w[2]=epoch;w[3]=last_error;
    w[4]=(u32)current.center_hz;w[5]=(u32)(current.center_hz>>32);
    w[6]=current.bit_rate;w[7]=current.bandwidth_hz;w[8]=(u32)current.gain_db;
    w[9]=current.mode;w[10]=current.line_code;w[11]=current.frame_bytes;w[12]=(u32)current.sync_word;
    w[26]=(u32)(current.sync_word>>32);w[27]=current.sync_bits;w[28]=current.frame_count;w[29]=current.sfid_start;
    if(p351_rx_dsp_capable() && p351_rx_timing_capable() && counter(7)==RXR_ABI){
        u32 retry;
        update_totals();w[13]=read_reg(RXR_STATUS);
        if(!stop_pending && (w[13]&12U))w[3]=(w[13]&8U)?8U:7U;
        w[14]=(u32)totals[0];w[15]=(u32)totals[1];
        w[16]=(u32)totals[3];w[17]=(u32)totals[2];
        w[20]=(u32)(totals[0]>>32);w[21]=(u32)(totals[1]>>32);
        w[22]=(u32)(totals[3]>>32);w[23]=(u32)(totals[2]>>32);
        if(!stop_pending){ /* 保留停止失败原因，不能让辅助快照错误覆盖它。 */
            Xil_Out32(RX2R_PACK_BASE+RXR_SELECT,4U); /* 同时锁存微调/NCO。 */
            for(retry=0;retry<1000U;retry++){
                if(read_reg(RXR_STATUS)&32U)break;
                usleep(1);
            }
            if(retry<1000U){w[24]=read_reg(RXR_COUNTER);w[25]=counter(5U);w[30]=counter(6U);}
            else w[3]=9U;
        }
    }else w[3]=2;
    w[18]=network_drops;w[19]=sequence;
}
