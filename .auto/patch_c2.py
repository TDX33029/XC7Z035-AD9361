"""udp_stack.c：新增 SDRQ（读屏幕草稿镜像）/ SDRW（写屏幕草稿变量）指令。"""
import re, os, shutil

P = r'D:\Document\tempPrj\XC7Z035-AD9361\P351\output\app_source\udp_stack.c'
d = open(P, encoding='utf-8', errors='replace').read()

# 0) 若上一版补丁已写入，先恢复备份重打
bak = P + '.bak'
import shutil
if not os.path.exists(bak):
    shutil.copyfile(P, bak)

# 1) include screen_uart0.h
a0 = '#include "app_upload.h"'
if '#include "screen_uart0.h"' not in d:
    assert a0 in d
    d = d.replace(a0, a0 + '\n#include "screen_uart0.h"', 1)

# 2) 两个处理函数：插在 udp_stack_rx_handle 之前
a1 = 'void udp_stack_rx_handle(const u8 *buf, u16 len)'
assert a1 in d
funcs = '''/* ---- 屏幕草稿镜像：PC 上位机屏幕为主同步（读全部+写单变量） ---- */
static void udp_screen_mirror_reply(const u8 *request, u32 ihl, u32 nonce)
{
    u8 *out = eth_tx_get_buf();
    u16 checksum;
    u32 k;
    memset(out, 0, 90U);
    memcpy(out, request + 6, 6U);
    memcpy(out + 6, local_mac, 6U);
    put_be16(out + 12, ETHTYPE_IPV4);
    out[14] = 0x45; out[22] = 64; out[23] = 17;
    put_be16(out + 16, (u16)(20U + 8U + 72U));
    memcpy(out + 26, board_ip, 4U);
    memcpy(out + 30, request + 26, 4U);
    checksum = ip_checksum(out + 14, 20U);
    put_be16(out + 24, checksum);
    put_be16(out + 34, UDP_PORT_CTRL);
    put_be16(out + 36, get_be16(request + 14U + ihl));
    put_be16(out + 38, (u16)(8U + 72U));   /* UDP 校验和保持 0（IPv4 允许） */
    memcpy(out + 42, "SDRS", 4U);
    put_le32(out + 46, nonce);
    put_le32(out + 50, SCREEN_MIRROR_VARS);
    for (k = 0U; k < SCREEN_MIRROR_VARS; ++k)
        put_le32(out + 54U + 4U * k, (u32)screen_uart0_mirror_value((u8)k));
    put_le32(out + 54U + 4U * SCREEN_MIRROR_VARS,
             (u32)screen_uart0_mirror_editing());
    eth_tx_send(out, (u16)(14U + 20U + 8U + 72U));
}

static void udp_screen_draft_write(const u8 *request, u32 ihl, const u8 *pl)
{
    u32 nonce = get_le32(pl + 4U);
    u32 idx = get_le32(pl + 8U);
    u32 val = get_le32(pl + 12U);
    u32 result;
    if (idx >= SCREEN_MIRROR_VARS) result = 1U;
    else if (screen_uart0_mirror_editing()) result = 2U;   /* 屏幕编辑中，暂不写 */
    else if (!screen_uart0_draft_write((u8)idx, (s32)val)) result = 3U;
    else result = 0U;
    udp_control_reply(request, ihl, "SDWK", nonce, result);
}

void udp_stack_rx_handle(const u8 *buf, u16 len)'''
d = d.replace(a1, funcs, 1)

# 3) 分发：挂在 FRFQ 行后
a2 = 'if (len-o==8U && memcmp(buf+o,"FRFQ",4)==0){udp_final_rf_reply(buf,ihl,get_le32(buf+o+4));return;}'
assert a2 in d
d = d.replace(a2, a2 + '\n        if (len-o==8U && memcmp(buf+o,"SDRQ",4)==0){udp_screen_mirror_reply(buf,ihl,get_le32(buf+o+4));return;}\n        if (len-o==16U && memcmp(buf+o,"SDRW",4)==0){udp_screen_draft_write(buf,ihl,buf+o);return;}', 1)

open(P, 'w', encoding='utf-8', newline='\n').write(d)
print('udp_stack.c patched')
