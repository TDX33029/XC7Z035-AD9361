/* 两级上变频事务：70+1430=1500MHz；LO2=最终频率-1500MHz。
 * LD/外衰减故障由PS轮询关断，并非PL硬件纳秒级联锁。外10M另有现有PL门控。
 * 预测功率来自离线标定，不是在线功率传感器；无数据/未获硬件资格拒绝发射。 */
#include "p351_up70_calibration.h"
static screen_command_t screen_up70_command;
static p351_up70_power_plan_t screen_up70_power;
static u32 screen_up70_phase,screen_up70_fault,screen_up70_expected_ext;
static u32 screen_up70_signature[BOARD_CONFIG_WORDS];
static XTime screen_up70_began,screen_up70_lo_began,screen_up70_locked,screen_up70_chip_checked;
static u8 screen_up70_lock_seen,screen_up70_committed;
static u8 screen_up70_pc;

int board_final_rf_owned(void)
{ return screen_up70_phase>=1U && screen_up70_phase<=4U; }

static u32 screen_up70_levels(u32 halfdb)
{ return halfdb>=63U ? ((~(halfdb-63U))&63U) : (64U|((~halfdb)&63U)); }

static u8 screen_up70_tfs_matches(void)
{
#ifdef TFS_BASE
    u64 v=(u64)Xil_In32(TFS_BASE+0x10U)|((u64)Xil_In32(TFS_BASE+0x14U)<<32);
    return Xil_In32(TFS_BASE+0x24U)==2U &&
        v==(u64)(screen_up70_command.arg[10]-1500000U)*10000ULL;
#else
    return 0U; /* 不用猜测的地址访问不存在的外设。 */
#endif
}

/* 同一纯读构造器供UART发布和UDP查询使用。不调用guard/shutdown，不操作SPI。
 * 已停止计划仅保留作“最后计划”；必须FR_VALID才能显示当前最终RF有效。 */
static void screen_up70_readback(u32 f[FINAL_RF_STATUS_WORDS],
                                 const u32 b[BOARD_CONFIG_WORDS], u32 s)
{
    u32 known=p351_up70_cal_frequency_valid(screen_up70_command.arg[10]);
    u32 reference=refclk_valid && p351_clock_ready() &&
        refclk_source_current==1U && refclk_rate_current==10000000U;
    u32 valid=known && screen_up70_committed && screen_up70_phase==4U &&
        !screen_up70_fault && P351_RF_HARDWARE_QUALIFIED && reference &&
        (b[BC_FLAGS]&BOARD_CONFIG_RF_VALID) && !(b[BC_FLAGS]&(64U|128U)) &&
        (s&0x300U)==0x300U && !(s&0x00ff0005U) && (s&2U) && (s&8U) &&
        screen_up70_tfs_matches() &&
        ((s>>24)&127U)==screen_up70_levels(screen_up70_expected_ext);
    u32 i;
    for(i=0U;valid && i<BOARD_CONFIG_WORDS;i++)
        if(i!=BC_FLAGS && i!=BC_BOOT_RESULT && b[i]!=screen_up70_signature[i]) valid=0U;
    memset(f,0,FINAL_RF_STATUS_WORDS*sizeof(*f));
    f[FR_VERSION]=FINAL_RF_STATUS_VERSION;
    f[FR_FLAGS]=(valid?FR_VALID:0U) | (board_final_rf_owned()?FR_OWNED:0U) |
        (P351_RF_HARDWARE_QUALIFIED?FR_QUALIFIED:0U) |
        (screen_up70_committed?FR_COMMITTED:0U) | (reference?FR_REFERENCE_OK:0U) |
        ((s&8U)?FR_TFS_LOCK:0U) | (known?FR_PLAN_KNOWN:0U);
#if P351_MANUAL_RF_TEST
    f[FR_FLAGS]|=FR_MANUAL_TEST;
#endif
    if(!board_final_rf_owned() && b[BC_TX1]!=TX_MODE_OFF && refclk_valid &&
       p351_clock_ready() && (b[BC_FLAGS]&BOARD_CONFIG_RF_VALID) && !(b[BC_FLAGS]&64U))
        f[FR_FLAGS]|=FR_DIRECT_ACTIVE;
    f[FR_PHASE]=screen_up70_phase; f[FR_FAULT]=screen_up70_fault;
    f[FR_FINAL_KHZ]=known?screen_up70_command.arg[10]:0U;
    f[FR_TARGET_MDBM]=known?(u32)(-(s32)screen_up70_command.arg[11]):0U;
    f[FR_EXPECT_MDBM]=known?(u32)screen_up70_power.predicted_final_mdbm:0U;
    f[FR_MODEL]=known?screen_up70_power.confidence:0U;
    f[FR_IF_KHZ]=(u32)((((u64)b[BC_LO_HIGH]<<32)|b[BC_LO_LOW])/1000ULL);
    f[FR_IF_TARGET_MDBM]=(u32)P351_IF70_TARGET_MDBM;
    f[FR_IF_ATT_MDB]=b[BC_ATT1];
    f[FR_EXT_HALFDB]=((~(s>>24))&63U)+((s&(64U<<24))?0U:63U);
#ifdef TFS_BASE
    if(s&0x100U) {
        u64 lo=(u64)Xil_In32(TFS_BASE+0x10U)|((u64)Xil_In32(TFS_BASE+0x14U)<<32);
        f[FR_LO2_KHZ]=(u32)(lo/10000ULL); /* PL请求寄存器，不是RF测频 */
    }
#endif
    f[FR_TFS_STATUS]=s; f[FR_MODE]=b[BC_TX1]; f[FR_RATE]=b[BC_RATE];
    f[FR_CODE]=b[BC_LINE]; f[FR_FRAMED]=b[BC_FRAMED];
    f[FR_FBYTES]=b[BC_FRAME_BYTES]; f[FR_FCOUNT]=b[BC_FRAME_COUNT];
    f[FR_SYNC_LOW]=b[BC_SYNC_LOW]; f[FR_SYNC_HIGH]=b[BC_SYNC_HIGH];
    f[FR_SYNC_BITS]=b[BC_SYNC_BITS]; f[FR_REF_SOURCE]=b[BC_REF_SOURCE];
    f[FR_REF_RATE]=b[BC_REF_RATE]; f[FR_TASK]=b[BC_TASK];
    f[FR_SEQUENCE]=known?screen_up70_command.sequence:0U;
    f[FR_BOARD_VERSION]=b[BC_VERSION]; f[FR_BOARD_FLAGS]=b[BC_FLAGS];
    f[FR_SOURCE]=b[BC_SOURCE]; f[FR_REPEAT]=b[BC_REPEAT];
}

void board_final_rf_snapshot(u32 f[FINAL_RF_STATUS_WORDS])
{
    u32 b[BOARD_CONFIG_WORDS];
    board_config_snapshot(b);
    screen_up70_readback(f,b,tfs_control_status());
}

static void screen_up70_shutdown(u32 fault)
{
    screen_up70_committed=0U;
    screen_up70_phase=5U; /* 先撤销所有权/有效性，关断不递归轮询网络。 */
    if(fault) screen_up70_fault=fault;
    screen_task_active=0U;
    /* 先关板载射频通路并令后级最大衰减，再等待可能超时的DMA清空。 */
    (void)p351_rf_route(0U);
    (void)tfs_control_request(4U,5U,126U);
    modulation_config_fail_closed();
    (void)tfs_control_request(3U,5U,0U);
    (void)tfs_control_request(4U,5U,126U);
    if(refclk_transaction_pending || p351_clock_busy()) {
        p351_clock_abort(); refclk_transaction_pending=0U; refclk_valid=0U;
    }
    screen_uart0_set_status(fault?"RF FAULT":"TX DONE");
}

static u8 screen_up70_guard(void)
{
    u32 s,b[BOARD_CONFIG_WORDS],i,q;
    XTime now;
    if(!board_final_rf_owned()) return 0U;
    if(udp_ctrl_tx_stop_pending() || screen_uart0_stop_pending_get()) {
        screen_up70_shutdown(1U); return 1U;
    }
    /* phase1等待外10M资格，不能把尚未锁定当作运行时失锁。 */
    if(screen_up70_phase==1U) return 0U;
    if(!P351_RF_HARDWARE_QUALIFIED || !refclk_valid || !p351_clock_ready() ||
       refclk_source_current!=1U || refclk_rate_current!=10000000U) {
        screen_up70_shutdown(2U); return 1U;
    }
    s=tfs_control_status();
    if((s&0x300U)!=0x300U || (s&0x00ff0004U) || !screen_up70_tfs_matches()) {
        screen_up70_shutdown(3U); return 1U;
    }
    if(((s>>24)&127U)!=screen_up70_levels(screen_up70_expected_ext)) {
        screen_up70_shutdown(5U); return 1U;
    }
    if(screen_up70_phase>=3U && ((s&1U) || !(s&2U) || !(s&8U))) {
        screen_up70_shutdown(4U); return 1U;
    }
    if(screen_up70_phase!=4U) return 0U;
    q=tx_bit_protocol_query(0x53000000U|screen_up70_command.sequence);
    if(!(q&64U) || (q&32U)) { screen_up70_shutdown(7U); return 1U; }
    if(!screen_up70_command.arg[6] && (q&4U) && !(q&(8U|16U))) {
        screen_up70_shutdown(0U); return 0U;
    }
    if(screen_up70_command.arg[6] && !(q&(8U|16U))) {
        screen_up70_shutdown(6U); return 1U;
    }
    board_config_snapshot(b);
    if(!(b[BC_FLAGS]&BOARD_CONFIG_RF_VALID) || (b[BC_FLAGS]&(64U|128U))) {
        screen_up70_shutdown(6U); return 1U;
    }
    for(i=0U;i<BOARD_CONFIG_WORDS;i++) {
        if(i!=BC_FLAGS && i!=BC_BOOT_RESULT && b[i]!=screen_up70_signature[i]) {
            screen_up70_shutdown(7U); return 1U;
        }
    }
    XTime_GetTime(&now);
    if(now-screen_up70_chip_checked>=(XTime)(COUNTS_PER_SECOND/10U)) {
        screen_up70_chip_checked=now;
        if(!screen_up70_chip_healthy(screen_up70_power.if_atten_mdb)) {
            screen_up70_shutdown(9U); return 1U;
        }
    }
    return 0U;
}

static void screen_up70_output_stage(u32 external)
{ if(screen_up70_phase==3U) screen_up70_expected_ext=external; }

static u8 screen_up70_interlock(u64 lo_hz,u32 rate,u32 attenuation,u32 external)
{
    if(screen_up70_phase!=3U || lo_hz!=70000000ULL ||
       rate!=screen_command_rate_hz(&screen_up70_command) ||
       attenuation!=screen_up70_power.if_atten_mdb ||
       external!=screen_up70_power.ext_half_db) return 0U;
    return !screen_up70_guard();
}

static u32 screen_up70_program_lo(void)
{
    u32 s;
    screen_up70_phase=2U; screen_up70_lock_seen=0U;
    XTime_GetTime(&screen_up70_lo_began);
    s=tfs_control_request(2U,5U,
        (u64)(screen_up70_command.arg[10]-1500000U)*10000ULL);
    if((s&0x300U)!=0x300U || (s&0x00ff0004U) || !screen_up70_tfs_matches()) {
        screen_up70_shutdown(3U); return SC_FAILED;
    }
    return SC_PENDING;
}

static u32 screen_up70_begin(const screen_command_t *c)
{
    p351_up70_power_plan_t p;
    u32 s;
    if(board_final_rf_owned() || rx_receiver_active() || refclk_transaction_pending || p351_clock_busy()) return SC_BUSY;
    /* 先计算完整可达功率预算；无标定/不可达不会破坏另一个端正在发射的任务。 */
    if(!P351_RF_HARDWARE_QUALIFIED || !p351_up70_power_plan(c->arg[0],c->arg[10],
        screen_command_rate_hz(c),-(s32)c->arg[11],&p)) {
        screen_uart0_set_status("NOT CAL"); return SC_NOT_READY;
    }
    s=tfs_control_status();
    if((s&0x300U)!=0x300U || (s&0x00ff0000U)) return SC_NOT_READY;
    if(s&1U) return SC_BUSY;
    screen_up70_command=*c; screen_up70_power=p;
    screen_up70_pc=screen_request_pc;
    screen_up70_committed=0U; screen_up70_fault=0U; screen_up70_phase=1U;
    screen_up70_expected_ext=126U;
    screen_if70_committed=0U; screen_system_committed=0U;
    /* 取消可能在旧UI留下的IQ采集，运行期间不进入同步大块UDP上传。 */
    udp_stream_force(0U); udp_ctrl_capture_clear();
    XTime_GetTime(&screen_up70_began);
    screen_task_active=0U;
    /* 旧任务也先从板载RF通路断开，再做可能等待的DMA中止。 */
    if(p351_rf_route(0U)!=0) { screen_up70_shutdown(6U); return SC_FAILED; }
    s=tfs_control_request(4U,5U,126U);
    modulation_config_fail_closed();
    if(!(s&0x200U) || (s&0x00ff0000U)) {
        screen_up70_shutdown(5U); return SC_FAILED;
    }
    if(screen_stop_requested()) return SC_STOPPED;
    if(refclk_valid && p351_clock_ready() && refclk_source_current==1U && refclk_rate_current==10000000U)
        return screen_up70_program_lo();
    s=refclk_apply_config(1U,10000000U);
    if(s==REFCLK_APPLY_PENDING) return SC_PENDING;
    if(s==0U && refclk_valid && p351_clock_ready()) return screen_up70_program_lo();
    screen_up70_shutdown(2U); return SC_NOT_READY;
}

static u32 screen_up70_service(void)
{
    XTime now;
    u32 s;
    screen_apply_t r={0};
    const u32 *a=screen_up70_command.arg;
    if(screen_up70_guard()) return screen_up70_fault==1U?SC_STOPPED:SC_FAILED;
    if(screen_up70_phase==5U) return screen_up70_fault?SC_FAILED:SC_OK;
    XTime_GetTime(&now);
    if(now-screen_up70_began>(XTime)COUNTS_PER_SECOND*15U) {
        screen_up70_shutdown(8U); return SC_TIMEOUT;
    }
    if(screen_up70_phase==1U) {
        if(refclk_transaction_pending || p351_clock_busy()) return SC_PENDING;
        if(!refclk_valid || !p351_clock_ready() || refclk_source_current!=1U ||
           refclk_rate_current!=10000000U) { screen_up70_shutdown(2U); return SC_FAILED; }
        return screen_up70_program_lo();
    }
    if(screen_up70_phase!=2U) return SC_PENDING;
    s=tfs_control_status();
    if((s&1U) || !(s&2U) || !(s&8U)) {
        screen_up70_lock_seen=0U;
        if(now-screen_up70_lo_began>(XTime)(COUNTS_PER_SECOND/1000U)*P351_SYSTEM_TIMEOUT_MS) {
            screen_up70_shutdown(4U); return SC_TIMEOUT;
        }
        return SC_PENDING;
    }
    if(!screen_up70_lock_seen) { screen_up70_locked=now; screen_up70_lock_seen=1U; }
    if(now-screen_up70_locked<(XTime)(COUNTS_PER_SECOND/1000U)*P351_UPCONVERTER_LOCK_STABLE_MS) return SC_PENDING;
    screen_up70_phase=3U;
    r.sequence=screen_up70_command.sequence; r.mode=a[0]; r.lo_mhz=70U;
    r.rate_mbps=screen_command_rate_hz(&screen_up70_command)/1000000U; r.line_code=a[2]; r.framed=a[3];
    r.frame_bytes=a[4]; r.frame_count=a[5]; r.infinite=a[6];
    r.sync_word_low=a[7]|(a[8]<<16); r.sync_word_high=(a[9]>>8)&1U; r.sync_bits=a[9]&255U;
    r.system_profile=2U; r.total_atten_halfdb=screen_up70_power.ext_half_db;
    r.rf_bandwidth_hz=P351_IF70_BANDWIDTH_HZ;
    if(screen_execute_tx(&r,70000000ULL,screen_command_rate_hz(&screen_up70_command),screen_up70_power.if_atten_mdb)!=XST_SUCCESS) {
        if(screen_up70_phase!=5U) screen_up70_shutdown(6U);
        return screen_up70_fault==1U?SC_STOPPED:SC_FAILED;
    }
    if(screen_up70_guard()) return SC_FAILED;
    if(!screen_up70_chip_healthy(screen_up70_power.if_atten_mdb)) {
        screen_up70_shutdown(9U); return SC_FAILED;
    }
    XTime_GetTime(&screen_up70_chip_checked);
    board_config_snapshot(screen_up70_signature);
    screen_tfs_khz=a[10]-1500000U;
    screen_up70_committed=1U; screen_up70_phase=4U;
    /* 完整任务签名和最终路径同时提交，绝不因SPI发送成功就显示RF ON。 */
    return screen_up70_guard()?SC_FAILED:SC_OK;
}
