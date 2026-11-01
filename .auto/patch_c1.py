"""screen_uart0.c/h：草稿镜像轮询（get 指令）+ 0x71 应答捕获 + 草稿写队列。"""
import re

H = r'D:\Document\tempPrj\XC7Z035-AD9361\P351\output\app_source\screen_uart0.h'
C = r'D:\Document\tempPrj\XC7Z035-AD9361\P351\output\app_source\screen_uart0.c'

# ---------- 头文件 ----------
d = open(H, encoding='utf-8', errors='replace').read()
anchor = 'u8 screen_uart0_v2_link(void);'
assert anchor in d
decls = '''u8 screen_uart0_v2_link(void);
/* 草稿镜像：PS 轮询屏幕草稿变量（get 指令），供 PC 上位机屏幕为主同步。 */
#define SCREEN_MIRROR_VARS 14U   /* df,dr,dpo,dm,dc,dfr,db,dn,di,dsl,dsh,dsb,dsx,edit */
void screen_uart0_mirror_poll(void);                 /* 主循环调用（内部自带节拍） */
u8   screen_uart0_mirror_fresh(u8 idx);              /* 该变量自上轮以来是否已刷新 */
s32  screen_uart0_mirror_value(u8 idx);              /* 最近一次 get 到的值 */
u8   screen_uart0_mirror_editing(void);              /* 屏幕 edit 标志（1=编辑中） */
u8   screen_uart0_draft_write(u8 idx, s32 value);    /* PC 草稿写：回写屏幕变量（0=排队成功）'''
d = d.replace(anchor, decls, 1)
open(H, 'w', encoding='utf-8', newline='\n').write(d)
print('screen_uart0.h patched')

# ---------- C 文件 ----------
c = open(C, encoding='utf-8', errors='replace').read()

# 1) 镜像状态变量（插在 screen_ack_sequence 声明后）
a1 = 'static u32 screen_ack_sequence, screen_ack_op, screen_ack_result;'
assert a1 in c
mirror_state = a1 + '''
/* ---- 草稿镜像（屏幕为主同步）---- */
#define SCREEN_MIRROR_VARS 14U
static const char *const screen_mirror_names[SCREEN_MIRROR_VARS] = {
    "df","dr","dpo","dm","dc","dfr","db","dn","di","dsl","dsh","dsb","dsx","edit"};
static s32 screen_mirror[SCREEN_MIRROR_VARS];
static u8  screen_mirror_fresh_mask;
static s8  screen_get_pending = -1;          /* 正在等待应答的变量序号 */
static XTime screen_get_last;
static struct { u8 used; u8 idx; s32 value; } screen_dw_queue[SCREEN_MIRROR_VARS];
static u8 screen_dw_head, screen_dw_tail;    /* PC 草稿写回队列 */'''
c = c.replace(a1, mirror_state, 1)

# 2) 0x71 应答捕获：screen_complete_frame 内加 0x71 分支（挂在 0x88 分支之前）
a2 = '    } else if (screen_rx[0] == 0x88U) {'
assert a2 in c
cap = '''    } else if (screen_rx[0] == 0x71U) {
        /* get 应答：0x71+LE32+FFFFFF；按未决序号入镜像 */
        if (screen_get_pending >= 0 && (u8)screen_get_pending < SCREEN_MIRROR_VARS) {
            screen_mirror[screen_get_pending] = (s32)get_le32(screen_rx + 1U);
            screen_mirror_fresh_mask |= (u8)(1U << screen_get_pending);
            screen_get_pending = -1;
        }
    } else if (screen_rx[0] == 0x88U) {'''
c = c.replace(a2, cap, 1)

# 3) 公开函数：镜像访问 + 草稿写 + 轮询（插在 screen_uart0_poll 之前）
a3 = 'void screen_uart0_poll(void)'
assert a3 in c
funcs = '''u8 screen_uart0_mirror_fresh(u8 idx)
{
    return (idx < SCREEN_MIRROR_VARS) ? ((screen_mirror_fresh_mask >> idx) & 1U) : 0U;
}
s32 screen_uart0_mirror_value(u8 idx)
{
    return (idx < SCREEN_MIRROR_VARS) ? screen_mirror[idx] : 0;
}
u8 screen_uart0_mirror_editing(void)
{
    return (screen_mirror[13] != 0U) ? 1U : 0U;
}
u8 screen_uart0_draft_write(u8 idx, s32 value)
{
    u8 slot = screen_dw_head;
    if (idx >= SCREEN_MIRROR_VARS || screen_dw_tail != screen_dw_head) return 0U;
    screen_dw_queue[slot].used = 1U; screen_dw_queue[slot].idx = idx;
    screen_dw_queue[slot].value = value;
    screen_dw_head = (u8)((screen_dw_head + 1U) % SCREEN_MIRROR_VARS);
    return 1U;
}
void screen_uart0_mirror_poll(void)
{
    XTime now;
    if (!screen_ready || !screen_link) { screen_get_pending = -1; return; }
    XTime_GetTime(&now);
    /* 草稿写回：优先于 get（每次 poll 至多一条指令入队） */
    if (screen_dw_head != screen_dw_tail && !screen_tx_len) {
        u8 slot = screen_dw_tail;
        char command[24];
        (void)snprintf(command, sizeof command, "%s=%ld",
                       screen_mirror_names[screen_dw_queue[slot].idx],
                       (long)screen_dw_queue[slot].value);
        screen_enqueue(command);
        screen_dw_tail = (u8)((screen_dw_tail + 1U) % SCREEN_MIRROR_VARS);
        return;
    }
    /* get 轮询：250ms 一变量，整表 3.5s */
    if (screen_get_pending < 0 && !screen_tx_len &&
        now - screen_get_last >= (XTime)(COUNTS_PER_SECOND / 4U)) {
        char command[24];
        (void)snprintf(command, sizeof command, "get %s", screen_mirror_names[screen_get_round]);
        screen_enqueue(command);
        screen_get_pending = (s8)screen_get_round;
        screen_get_round = (u8)((screen_get_round + 1U) % SCREEN_MIRROR_VARS);
        screen_get_last = now;
    }
}
void screen_uart0_poll(void)'''
c = c.replace(a3, funcs, 1)

# 4) 轮询内声明补充：screen_get_round 变量 + poll 内调用 mirror_poll（放函数体首）
a4 = '    XTime now;\n    u32 i;\n    if (!screen_ready)\n        return;'
assert a4 in c
c = c.replace(a4, '''    static u8 screen_get_round;
    XTime now;
    u32 i;
    if (!screen_ready)
        return;''', 1)
a5 = '''    if (!screen_ready)
        return;
    XTime_GetTime(&now);
    if (screen_rx_len && now - screen_last_rx_byte >'''
assert a5 in c
c = c.replace(a5, '''    if (!screen_ready)
        return;
    screen_uart0_mirror_poll();
    XTime_GetTime(&now);
    if (screen_rx_len && now - screen_last_rx_byte >''', 1)

# 5) link 断开/重启时复位 get 未决
a6 = '''        screen_link = 0U;
        screen_text_pending = 0U;'''
assert a6 in c
c = c.replace(a6, '''        screen_link = 0U;
        screen_get_pending = -1;
        screen_text_pending = 0U;''', 1)
a7 = '''        if(screen_rf_watchdog) screen_stop_pending=1U;
        screen_link = 0U;
        screen_cw_armed = 0U;'''
assert a7 in c
c = c.replace(a7, '''        if(screen_rf_watchdog) screen_stop_pending=1U;
        screen_link = 0U;
        screen_get_pending = -1;
        screen_cw_armed = 0U;''', 1)

# 6) 镜像表清零放 init（screen_ready=1 前后）
a8 = '    screen_ack_sequence=screen_ack_op=screen_ack_result=0;'
assert a8 in c
c = c.replace(a8, a8 + '\n    memset(screen_mirror,0,sizeof screen_mirror); screen_mirror_fresh_mask=0U;\n    screen_get_pending=-1; screen_dw_head=screen_dw_tail=0U;', 1)

open(C, 'w', encoding='utf-8', newline='\n').write(c)
print('screen_uart0.c patched; get_le32 available:', 'get_le32' in c)
