/* main.c专用前面板业务：共用已有RF/本地PN11/接收/参考源保护，不在UART收字节时执行。
 * PN11固定7FF；v1/v2默认同步FE6B2840/32bit，v3原子传入16..33bit同步字。
 * 复帧SFID从0开始；整机频率/总外衰减策略不冒充缺失的射频混频/旁路硬件。 */
#include "tfs_control.h"
#include "p351_tx_system.h"
#include "p351_if70_profile.h"
static screen_command_t screen_async;
static u8 screen_async_active;
static u8 screen_request_pc, screen_async_pc;
static XTime screen_async_began;
static u32 screen_tfs_khz; /* 成功SPI事务的请求值，非频率实测值 */
static p351_tx_plan_t screen_system_plan;
static u8 screen_system_committed, screen_system_lock_seen;
static u32 screen_system_task;
static XTime screen_system_locked_at;
static u32 screen_system_signature[BOARD_CONFIG_WORDS], screen_system_tfs_signature[3];
static u8 screen_if70_committed, screen_if70_mode;
static u32 screen_if70_signature[BOARD_CONFIG_WORDS];
#include "screen_up70_controller.inc"

/* 新前面板固定IF事务不操作TFS/外衰减器，也不提供内部40MHz旁路。
 * 资格与逐调制实测功率未完成时，拒绝请求且不会停止别的控制端当前任务。 */
static u32 screen_if70_finish(const screen_command_t *c)
{
    const u32 *a=c->arg;
    screen_apply_t r={0};
    if(!P351_RF_HARDWARE_QUALIFIED || !p351_if70_power_calibrated(a[0]) ||
       !refclk_valid || !p351_clock_ready() || refclk_source_current!=1U ||
       refclk_rate_current!=10000000U) return SC_NOT_READY;
    if(screen_stop_requested()) return SC_STOPPED;
    r.sequence=c->sequence; r.mode=a[0]; r.lo_mhz=70U;
    r.rate_mbps=a[1]/1000U; r.line_code=a[2]; r.framed=a[3];
    r.frame_bytes=a[4]; r.frame_count=a[5]; r.infinite=a[6];
    r.sync_word_low=a[7]|(a[8]<<16); r.sync_word_high=(a[9]>>8)&1U;
    r.sync_bits=a[9]&255U;
    r.rf_bandwidth_hz=P351_IF70_BANDWIDTH_HZ;
    screen_if70_committed=0U;
    if(screen_execute_tx(&r,P351_IF70_CENTER_HZ,a[1]*1000U,
                        p351_if70_attenuation_mdb(a[0]))!=XST_SUCCESS) return SC_FAILED;
    screen_if70_mode=(u8)a[0];
    board_config_snapshot(screen_if70_signature);
    screen_if70_committed=1U;
    return SC_OK;
}

static u32 screen_if70_begin(const screen_command_t *c)
{
    u32 s;
    if(rx_receiver_active()) return SC_BUSY;
    if(!P351_RF_HARDWARE_QUALIFIED || !p351_if70_power_calibrated(c->arg[0])) {
        screen_uart0_set_status("NOT CAL"); return SC_NOT_READY;
    }
    if(refclk_valid && p351_clock_ready() && refclk_source_current==1U &&
       refclk_rate_current==10000000U) return screen_if70_finish(c);
    /* 失锁后只在用户明确重新APPLY时重做外10M资格；不自动恢复RF。
     * 因而删除时钟页后仍能恢复，但不会选择内部40M或静默降级。 */
    s=refclk_apply_config(1U,10000000U);
    if(s==REFCLK_APPLY_PENDING) return SC_PENDING;
    return s==0U ? screen_if70_finish(c) : SC_NOT_READY;
}

static u8 screen_if70_observe(const u32 b[BOARD_CONFIG_WORDS])
{
    u32 i;
    if(!screen_if70_committed) return 0U;
    if(!refclk_valid || !p351_clock_ready() || refclk_source_current!=1U ||
       !(b[BC_FLAGS]&BOARD_CONFIG_RF_VALID) || (b[BC_FLAGS]&128U) ||
       !p351_if70_power_calibrated(screen_if70_mode)) screen_if70_committed=0U;
    for(i=0U;screen_if70_committed && i<BOARD_CONFIG_WORDS;++i) {
        if(i!=BC_FLAGS && i!=BC_BOOT_RESULT && b[i]!=screen_if70_signature[i])
            screen_if70_committed=0U;
    }
    return screen_if70_committed;
}

/* 只读PL中最近请求的参数/操作码，不冒充TFS芯片或RF测频读回。
 * 生产BSP存在TFS_BASE时才访问；无该IP的BSP已由status/present保护拒绝整机事务。 */
static void screen_system_tfs_snapshot(u32 words[3])
{
    words[0]=words[1]=words[2]=0U;
#ifdef TFS_BASE
    words[0]=Xil_In32(TFS_BASE+0x10U);
    words[1]=Xil_In32(TFS_BASE+0x14U);
    words[2]=Xil_In32(TFS_BASE+0x24U);
#endif
}

static u8 screen_system_tfs_plan_matches(const p351_tx_plan_t *p)
{
#ifdef TFS_BASE
    u32 words[3];
    u64 value;
    if(p->path!=P351_PATH_UPCONVERT) return 1U;
    screen_system_tfs_snapshot(words);
    value=((u64)words[1]<<32)|words[0];
    return words[2]==2U && value==(u64)p->tfs_lo_khz*10000ULL;
#else
    (void)p;
    return 1U; /* host无AXI模型；生产无TFS的BSP不能通过present检查。 */
#endif
}

static u8 screen_system_tfs_healthy(u32 s, const p351_tx_plan_t *p)
{
    return (s&0x300U)==0x300U && !(s&0x00ff0005U) &&
           (p->path!=P351_PATH_UPCONVERT || (s&8U));
}

/* 不依赖1秒发布器“碰巧看到”瞬时无效状态。旧控制端接管会立即清commit；
 * UDP/旧TX改变实际模式、任务、帧、驱动或参考，也由完整稳定签名发现。
 * 即使旧端写回相同值，已清除的commit也不会被自动重建。 */
static u8 screen_system_observe(const u32 b[BOARD_CONFIG_WORDS], u32 s)
{
    u32 i, words[3];
    u8 same_owner, matches, tfs_ok;
    if(!screen_system_committed) return 0U;
    screen_system_tfs_snapshot(words);
    tfs_ok=screen_system_tfs_healthy(s,&screen_system_plan) &&
        !memcmp(words,screen_system_tfs_signature,sizeof words);
    same_owner=b[BC_TX1]==screen_system_signature[BC_TX1] &&
        b[BC_LO_LOW]==screen_system_signature[BC_LO_LOW] &&
        b[BC_LO_HIGH]==screen_system_signature[BC_LO_HIGH] &&
        b[BC_ATT1]==screen_system_signature[BC_ATT1] &&
        (b[BC_TX1]==TX_MODE_CW || b[BC_TASK]==screen_system_task);
    matches=refclk_valid && p351_clock_ready() &&
        (b[BC_FLAGS]&BOARD_CONFIG_RF_VALID) && !(b[BC_FLAGS]&128U) &&
        (!P351_FINAL_REQUIRE_EXTERNAL_10M || b[BC_REF_SOURCE]==1U) &&
        !p351_tx_plan_blockers(&screen_system_plan) && tfs_ok;
    for(i=0U; matches && i<BOARD_CONFIG_WORDS; ++i) {
        /* FLAGS含任务活动/能力，BOOT_RESULT与当前RF映射无关；其他稳定配置逐字比较。 */
        if(i!=BC_FLAGS && i!=BC_BOOT_RESULT && b[i]!=screen_system_signature[i]) matches=0U;
    }
    if(!matches) {
        screen_system_committed=0U;
        /* 只停止仍属于该整机计划的输出；旧TX接管后不误杀另一端的新任务。
         * TFS忙/失锁/被改频期间，不能继续声称原上变频输出安全有效。 */
        if(same_owner && !tfs_ok) {
            modulation_config_fail_closed();
            (void)tfs_control_request(4U,5U,126U);
            screen_uart0_set_status("RF PATH LOST");
        }
    }
    return matches;
}

static u32 screen_system_finish(const screen_command_t *c, const p351_tx_plan_t *p)
{
    const u32 *a=c->arg;
    screen_apply_t r={0};
    r.sequence=c->sequence; r.mode=a[0]; r.lo_mhz=p->ad_lo_khz/1000U;
    r.attenuation_db=P351_DRIVER_FIXED_ATTEN_MDB/1000U; r.rate_mbps=a[3]/1000U;
    r.line_code=a[4]; r.framed=a[5]; r.frame_bytes=a[6];
    r.frame_count=a[7]; r.infinite=a[8];
    r.sync_word_low=a[9] | (a[10]<<16); r.sync_word_high=(a[11]>>8)&1U;
    r.sync_bits=a[11]&255U; r.system_profile=1; r.total_atten_halfdb=a[2];
    if (screen_stop_requested() || !refclk_valid || !p351_clock_ready() ||
        (P351_FINAL_REQUIRE_EXTERNAL_10M && refclk_source_current!=1U) ||
        p351_tx_plan_blockers(p) || !screen_system_tfs_plan_matches(p)) return SC_NOT_READY;
    if(screen_execute_tx(&r,(u64)p->ad_lo_khz*1000U,a[3]*1000U,
                         P351_DRIVER_FIXED_ATTEN_MDB)!=XST_SUCCESS) return SC_FAILED;
    /* screen_execute_tx内部会轮询网口；旧UDP可能在这段时间写了TFS。
     * 完整APPLY返回后再次核对，绝不把已被改写的本振计划提交为sysvalid。 */
    if(!screen_system_tfs_healthy(tfs_control_status(),p) || !screen_system_tfs_plan_matches(p)) {
        modulation_config_fail_closed();
        (void)tfs_control_request(4U,5U,126U);
        return SC_FAILED;
    }
    screen_system_plan=*p; screen_system_task=0x53000000U|c->sequence;
    board_config_snapshot(screen_system_signature);
    screen_system_tfs_snapshot(screen_system_tfs_signature);
    screen_system_committed=1;
    return SC_OK;
}

static u32 screen_system_begin(const screen_command_t *c)
{
    p351_tx_plan_t p;
    u32 s;
    if(!p351_tx_plan(c->arg[0],c->arg[1],&p)) return SC_PARAM;
    if(rx_receiver_active() || refclk_transaction_pending || p351_clock_busy()) return SC_BUSY;
    /* 未确认/未标定时拒绝，不去写“最终RF频率”到裸TFS本振，也不假装有RF旁路脚。 */
    if(p351_tx_plan_blockers(&p) || !refclk_valid || !p351_clock_ready() ||
       (P351_FINAL_REQUIRE_EXTERNAL_10M && refclk_source_current!=1U)) {
        screen_uart0_set_status("RF UNQUAL"); return SC_NOT_READY;
    }
    s=tfs_control_status();
    if((s&0x300U)!=0x300U || (s&0x00ff0000U)) return SC_NOT_READY;
    if(s&1U) return SC_BUSY;
    screen_system_committed=0;
    modulation_config_fail_closed();
    s=tfs_control_request(4U,5U,126U); /* RF关闭后，先最大衰减保护整个换频/校准事务 */
    if(!(s&0x200U) || (s&0x00ff0000U)) return SC_FAILED;
    if(screen_stop_requested()) return SC_STOPPED;
    if(p.path==P351_PATH_DIRECT) return screen_system_finish(c,&p);
    /* 已核准低侧LO/和频组件才写3725MHz；SPI频率是本振，不是最终调制信号频率。 */
    s=tfs_control_request(2U,5U,(u64)p.tfs_lo_khz*10000ULL);
    if(!(s&0x100U) || (s&0x00ff0004U)) return SC_FAILED;
    screen_system_plan=p; screen_system_lock_seen=0;
    return SC_PENDING;
}

static void screen_v2_cancel(void)
{
    if(board_final_rf_owned()) screen_up70_shutdown(1U);
    screen_if70_committed=0U;
    if(screen_system_committed || (screen_async_active && screen_async.op==SC_RF_TX)) {
        screen_system_committed=0;
        modulation_config_fail_closed();
        (void)tfs_control_request(4U,5U,126U);
    }
    if(!screen_async_active) return;
    if(screen_async.op==SC_REF || screen_async.op==SC_IF70_TX) {
        p351_clock_abort(); refclk_transaction_pending=0; refclk_valid=0;
        modulation_config_fail_closed();
    } else (void)tfs_control_request(3,5,0);
    if(!screen_async_pc) screen_uart0_ack(screen_async.sequence,screen_async.op,SC_STOPPED);
    udp_ctrl_final_rf_complete(screen_async.sequence,screen_async.op,SC_STOPPED);
    screen_async_active=0; screen_uart0_set_busy(0);
}

static u32 screen_v2_execute(const screen_command_t *c)
{
    const u32 *a;
    u32 s;
    if(!screen_command_valid(c)) return SC_PARAM;
    a=c->arg;
    if(c->op==SC_QUERY) return SC_OK;
    /* 旧TFT的可变LO/衰减/参考/TFS/RX写命令不得绕过固定70MHz前面板约定。
     * 旧包仍可解析/回ACK，便于明确发现版本不配套，而不是静默误执行。 */
    if(c->op!=SC_IF70_TX && c->op!=SC_UP70_TX && c->op!=SC_UP70_PRECISE_TX) return SC_NOT_READY;
    if(board_final_rf_owned()) return SC_BUSY;
    if(screen_async_active || refclk_transaction_pending || p351_clock_busy()) return SC_BUSY;
    if(screen_stop_requested()) return SC_STOPPED;
    screen_async_pc=screen_request_pc;
    /* 旧协议写操作不继承整机计划资格；各分支在忙态检查后、真正操作前清除。
     * 纯查询/已拒绝的BUSY不改变旧任务资格；旧控制执行后绝不自动重建commit。 */
    switch(c->op) {
    case SC_UP70_TX: case SC_UP70_PRECISE_TX:
        s=screen_up70_begin(c);
        if(s!=SC_PENDING) return s;
        break;
    case SC_IF70_TX:
        s=screen_if70_begin(c);
        if(s!=SC_PENDING) return s;
        break;
    case SC_RF_TX:
        s=screen_system_begin(c);
        if(s!=SC_PENDING) return s;
        break;
    case SC_TX: {
        screen_apply_t r={c->sequence,a[0],a[1]/1000U,a[2]/4U,a[3]/1000U,
                          a[4],a[5],a[6],a[7],a[8]};
        if(rx_receiver_active()) return SC_BUSY;
        screen_system_committed=0U;
        return screen_execute_tx(&r,(u64)a[1]*1000U,a[3]*1000U,a[2]*250U)==
               XST_SUCCESS ? SC_OK : SC_FAILED;
    }
    case SC_REF:
        if(rx_receiver_active()) return SC_BUSY;
        screen_system_committed=0U;
        s=refclk_apply_config((u8)a[0],a[0]?10000000U:40000000U);
        if(s!=REFCLK_APPLY_PENDING) return s ? SC_FAILED : SC_OK;
        break;
    case SC_TFS:
        if(tx1_mode_current!=TX_MODE_OFF || rx_receiver_active()) return SC_BUSY;
        screen_system_committed=0U;
        s=tfs_control_request(2,5,((u64)a[1]*65536U+a[0])*10000ULL);
        if(!(s&0x100U) || (s&0x00ff0004U)) return SC_FAILED;
        break;
    case SC_ATT:
        screen_system_committed=0U;
        s=tfs_control_request(4,5,a[0]);
        return (s&0x200U) && !(s&0x00ff0000U) ? SC_OK : SC_FAILED;
    case SC_TFS_READ:
        /* 有动作的读取/中止会占用TFS参数寄存器，也取消旧整机承诺。 */
        if(a[0]!=0U) screen_system_committed=0U;
        s=tfs_control_request(a[0]==0?0:a[0]==1?0x82:a[0]==2?0x86:3,5,0);
        if(!(s&0x100U) || (s&0x00ff0004U)) return SC_FAILED;
        if(a[0]==0 || a[0]==3) return SC_OK;
        break;
    case SC_RX: {
        rx_receiver_config_t r;
        screen_system_committed=0U;
        memset(&r,0,sizeof r);
        r.enabled=1; r.mode=(u8)a[0]; r.center_hz=(u64)a[1]*1000U;
        r.bit_rate=a[2]*1000U; r.bandwidth_hz=a[3]*1000U;
        r.gain_db=(s32)a[4]; r.line_code=(u8)a[5];
        r.frame_bytes=(u16)a[6]; r.frame_count=(u16)a[7];
        r.sync_word=UINT64_C(0xFE6B2840); r.sync_bits=32;
        return rx_receiver_apply(&r)==0 ? SC_OK : SC_FAILED;
    }
    case SC_RX_STOP: return rx_receiver_stop()==0 ? SC_OK : SC_FAILED;
    default: return SC_PARAM;
    }
    screen_async=*c; screen_async_active=1;
    XTime_GetTime(&screen_async_began); screen_uart0_set_busy(1);
    return SC_PENDING;
}

/* PC整机入口与屏幕共用同一个执行器；不复用屏幕ACK/失联所有权。 */
u32 board_pc_final_rf_execute(const screen_command_t *c)
{
    u32 rc;
    screen_request_pc=1U;
    rc=screen_v2_execute(c);
    screen_request_pc=0U;
    return rc;
}
u8 board_screen_rf_owned(void)
{
    return (board_final_rf_owned() && !screen_up70_pc) ||
           (screen_async_active && !screen_async_pc) || (screen_task_active && !screen_async_pc);
}

static void screen_v2_service(void)
{
    screen_command_t c;
    XTime now;
    u32 rc=SC_PENDING,s;
    (void)screen_up70_guard();
    if(screen_system_committed) {
        u32 b[BOARD_CONFIG_WORDS];
        board_config_snapshot(b);
        (void)screen_system_observe(b,tfs_control_status());
    }
    if(screen_async_active) {
        XTime_GetTime(&now);
        if(screen_stop_requested()) { screen_v2_cancel(); return; }
        if(screen_async.op==SC_UP70_TX || screen_async.op==SC_UP70_PRECISE_TX) {
            rc=screen_up70_service();
        } else if(screen_async.op==SC_IF70_TX) {
            if(!refclk_transaction_pending && !p351_clock_busy())
                rc=screen_if70_finish(&screen_async);
        } else if(screen_async.op==SC_REF) {
            if(!refclk_transaction_pending && !p351_clock_busy())
                rc=refclk_valid && p351_clock_ready() &&
                   refclk_source_current==screen_async.arg[0] ? SC_OK : SC_FAILED;
        } else if(screen_async.op==SC_RF_TX) {
            s=tfs_control_status();
            if(!(s&0x100U) || (s&0x00ff0004U)) rc=SC_FAILED;
            else if(!screen_system_tfs_plan_matches(&screen_system_plan)) rc=SC_FAILED;
            else if(!(s&1U) && (s&2U)) {
                if(s&8U) {
                    if(!screen_system_lock_seen) {
                        screen_system_locked_at=now; screen_system_lock_seen=1;
                    }
                    if(now-screen_system_locked_at >=
                       (XTime)(COUNTS_PER_SECOND/1000U)*P351_UPCONVERTER_LOCK_STABLE_MS) {
                        screen_tfs_khz=screen_system_plan.tfs_lo_khz;
                        rc=screen_system_finish(&screen_async,&screen_system_plan);
                    }
                } else screen_system_lock_seen=0;
            }
            if(rc==SC_PENDING && now-screen_async_began >
               (XTime)(COUNTS_PER_SECOND/1000U)*P351_SYSTEM_TIMEOUT_MS) rc=SC_TIMEOUT;
        } else {
            s=tfs_control_status();
            if(!(s&0x100U) || (s&0x00ff0004U)) rc=SC_FAILED;
            else if(!(s&1U) && (s&2U)) rc=SC_OK;
        }
        if(rc==SC_PENDING && now-screen_async_began>(XTime)COUNTS_PER_SECOND*15U) {
            if(screen_async.op==SC_REF || screen_async.op==SC_IF70_TX) {
                p351_clock_abort(); refclk_valid=0; refclk_transaction_pending=0;
                modulation_config_fail_closed();
            } else (void)tfs_control_request(3,5,0);
            rc=SC_TIMEOUT;
        }
        if(rc!=SC_PENDING) {
            if(screen_async.op==SC_RF_TX && rc!=SC_OK) {
                screen_system_committed=0;
                modulation_config_fail_closed();
                (void)tfs_control_request(3U,5U,0U);
                (void)tfs_control_request(4U,5U,126U);
            }
            if(rc==SC_OK && screen_async.op==SC_TFS)
                screen_tfs_khz=screen_async.arg[1]*65536U+screen_async.arg[0];
            if(!screen_async_pc) screen_uart0_ack(screen_async.sequence,screen_async.op,rc);
            udp_ctrl_final_rf_complete(screen_async.sequence,screen_async.op,rc);
            screen_async_active=0; screen_uart0_set_busy(0);
        }
    }
    if(!screen_async_active && screen_uart0_take_command(&c))
        screen_uart0_ack(c.sequence,c.op,screen_v2_execute(&c));
}

/* 全局只读变量与草稿分离。rev=0表示快照传输中，最后提交非零rev。
 * ackseq与实际配置同一次快照，屏幕不能仅凭发送成功显示RF已开启。 */
static void screen_v2_publish(void)
{
    static const char *names[]={"ackseq","ackop","ackrc","rfmode","flo","rate",
        "att","code","framed","fbytes","fcount","runflag","refsrc","refok",
        "clkstate","tfsstat","tfkhz","extatt","tempmc","tempok","rxon","rxf",
        "rxrate","rxbw","rxgain","rxmode","rxcode","rxstat","rxbits","rxerrs",
        "rxframes","countsat","qualified","rfvalid","source",
        "outkhz","route","sysvalid","adfix","syncbits","synclo","synchi","syncbit","totalatt",
        "pcmpcal","soqpcal","pcal","ifvalid","ifkhz","iftarget","ifatten",
        "rfpmag","rfexpect","rfmodel","rffault","rfphase"};
    static u32 values[sizeof names/sizeof names[0]],revision,index=99;
    static XTime last;
    XTime now;
    u32 b[BOARD_CONFIG_WORDS],r[RXR_CFG_WORDS],f[FINAL_RF_STATUS_WORDS],s,i=0,seq,op,rc,pins;
    u32 pcm_cal,soq_cal,mode_cal,up_if_valid;
    p351_up70_if_plan_t if_plan;
    s32 temp;
    if(!screen_uart0_v2_link()) { index=99; return; }
    XTime_GetTime(&now);
    if(index==99) {
        if(revision && now-last<COUNTS_PER_SECOND) return;
        board_config_snapshot(b); rx_receiver_snapshot(r);
        screen_uart0_ack_get(&seq,&op,&rc);
        s=tfs_control_status(); temp=board_temperature_mC();
        screen_up70_readback(f,b,s);
        pcm_cal=p351_if70_power_calibrated(0U);
        soq_cal=p351_if70_power_calibrated(1U);
        mode_cal=p351_if70_power_calibrated(screen_if70_mode);
        up_if_valid=(f[FR_FLAGS]&FR_VALID) && f[FR_IF_KHZ]==70000U;
        /* 新整机标定表不能沿用旧BOOT/IF70标定标志；模型不冒充逐范围实测。 */
        if(f[FR_FLAGS]&FR_PLAN_KNOWN) {
            pcm_cal=p351_up70_if_plan(0U,b[BC_RATE],&if_plan) && if_plan.measured;
            soq_cal=p351_up70_if_plan(1U,b[BC_RATE],&if_plan) && if_plan.measured;
            mode_cal=screen_up70_command.arg[0]?soq_cal:pcm_cal;
        }
        values[i++]=seq; values[i++]=op; values[i++]=rc; values[i++]=b[BC_TX1];
        values[i++]=(u32)((((u64)b[BC_LO_HIGH]<<32)|b[BC_LO_LOW])/1000U);
        values[i++]=b[BC_RATE]; values[i++]=b[BC_ATT1]/250U;
        values[i++]=b[BC_LINE]; values[i++]=b[BC_FRAMED];
        values[i++]=b[BC_FRAME_BYTES]; values[i++]=b[BC_FRAME_COUNT];
        /* runflag供屏幕回填“单次/无限”，不能用任务active冒充重复方式。 */
        values[i++]=b[BC_REPEAT]==UINT32_C(0xFFFFFFFF)?1U:0U; values[i++]=b[BC_REF_SOURCE];
        values[i++]=b[BC_FLAGS]&1U; values[i++]=p351_clock_state();
        /* 状态字高位TTL可能超过有符号int，单独extatt字段后只传低24位。 */
        values[i++]=s&0xffffffU; values[i++]=screen_tfs_khz;
        pins=(s>>24)&127U;
        values[i++]=((~pins)&63U)+((pins&64U)?0U:63U);
        values[i++]=temp>=0?(u32)temp:0; values[i++]=temp>=0;
        values[i++]=rx_receiver_active();
        values[i++]=(u32)((((u64)r[5]<<32)|r[4])/1000U);
        values[i++]=r[6]; values[i++]=r[7]; values[i++]=r[8];
        values[i++]=r[9]; values[i++]=r[10]; values[i++]=r[13];
        values[i++]=(r[23] || r[17]>2147483647U)?2147483647U:r[17];
        values[i++]=(r[22] || r[16]>2147483647U)?2147483647U:r[16];
        values[i++]=(r[21] || r[15]>2147483647U)?2147483647U:r[15];
        values[i++]=(r[21] || r[22] || r[23] || r[17]>2147483647U ||
                     r[16]>2147483647U || r[15]>2147483647U)?1U:0U;
        values[i++]=(b[BC_FLAGS]&BOARD_CONFIG_HW_QUALIFIED)!=0;
        values[i++]=(b[BC_FLAGS]&BOARD_CONFIG_RF_VALID)!=0;
        values[i++]=b[BC_SOURCE];
        /* 只发布已成功应用且仍与实际寄存器/任务缓存一致的最终频率。
         * PC/旧协议接管、改本振/衰减/同步后不沿用旧整机映射冒充新RF状态。 */
        {
            u8 matches=screen_system_observe(b,s);
            u8 up=(f[FR_FLAGS]&FR_VALID)!=0U;
            values[i++]=up?f[FR_FINAL_KHZ]:(matches?screen_system_plan.final_khz:0U);
            values[i++]=up?2U:(matches?screen_system_plan.path:0U);
            values[i++]=up?1U:(matches?1U:0U);
            values[i++]=P351_DRIVER_FIXED_ATTEN_MDB/250U;
        }
        values[i++]=b[BC_SYNC_BITS];
        values[i++]=b[BC_SYNC_LOW]&65535U;
        values[i++]=b[BC_SYNC_LOW]>>16;
        values[i++]=b[BC_SYNC_HIGH]&1U;
        values[i++]=((~pins)&63U)+((pins&64U)?0U:63U);
        values[i++]=pcm_cal;
        values[i++]=soq_cal;
        values[i++]=mode_cal;
        values[i++]=up_if_valid?1U:screen_if70_observe(b);
        values[i++]=(up_if_valid || screen_if70_committed)?70000U:0U;
        values[i++]=(u32)(-P351_IF70_TARGET_MDBM); /* 仅目标绝对值，不是功率实测值 */
        values[i++]=up_if_valid?f[FR_IF_ATT_MDB]/250U:p351_if70_attenuation_mdb(screen_if70_mode)/250U;
        values[i++]=(f[FR_FLAGS]&FR_VALID)?(u32)(-(s32)f[FR_TARGET_MDBM]):0U;
        values[i++]=(f[FR_FLAGS]&FR_VALID)?(u32)(-(s32)f[FR_EXPECT_MDBM]):0U;
        values[i++]=(f[FR_FLAGS]&FR_VALID)?f[FR_MODEL]:0U;
        values[i++]=f[FR_FAULT];
        values[i++]=f[FR_PHASE];
        if(++revision>2147483647U) revision=1;
        index=0; last=now;
    }
    if(!index) { if(screen_uart0_write_number("rev",0)) ++index; }
    else if(index<=sizeof names/sizeof names[0]) {
        if(screen_uart0_write_number(names[index-1],values[index-1])) ++index;
    } else if(screen_uart0_write_number("rev",revision)) index=99;
}
