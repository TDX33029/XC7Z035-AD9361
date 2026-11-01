/* 轻量 DHCP 客户端（RFC2131 子集，全广播，静态回退）。
 * 仅依赖 eth_link 帧接口与 udp_stack 地址接口；自带校验和辅助。 */
#include "udp_dhcp.h"
#include "udp_stack.h"
#include "eth_link.h"
#include "xil_printf.h"
#include <string.h>

#define DHCP_SERVER_PORT 67U
#define DHCP_CLIENT_PORT 68U
#define DHCP_MAGIC       0x63825363U
#define DHCP_OP_REQUEST  1U
#define DHCP_OP_REPLY    2U
#define DHCP_MSG_DISCOVER 1U
#define DHCP_MSG_OFFER    2U
#define DHCP_MSG_REQUEST  3U
#define DHCP_MSG_ACK      5U
#define DHCP_OPT_SUBNET   1U
#define DHCP_OPT_ROUTER   3U
#define DHCP_OPT_MSGTYPE  53U
#define DHCP_OPT_REQIP    50U
#define DHCP_OPT_SERVERID 54U
#define DHCP_OPT_END      255U
#define DHCP_ATTEMPTS     3U
#define DHCP_PAYLOAD      300U

extern void udp_stack_set_addresses(const u8 *ip, const u8 *mask, const u8 *gw);

static u8  dhcp_state;                 /* 1 discover, 2 request, 3 done, 4 fallback */
static u32 dhcp_xid;
static u8  dhcp_server[4];
static u8  dhcp_offer_ip[4];
static u32 dhcp_ticks;                 /* 100ms 节拍计数 */
static u8  dhcp_attempts = DHCP_ATTEMPTS;

static void dput_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u16 dip_checksum(const u8 *p, u32 n)
{
    u32 sum = 0;
    while (n > 1U) { sum += (u32)p[0] << 8 | p[1]; p += 2; n -= 2U; }
    if (n) sum += (u32)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (u16)(~sum & 0xFFFFU);
}

static u8 *dhcp_frame(void)
{
    u8 *buf = eth_tx_get_buf();
    if (!buf) return 0;
    memset(buf, 0, 14U + 20U + 8U + DHCP_PAYLOAD);
    memset(buf, 0xFF, 6U);                       /* 广播目的 MAC */
    buf[6] = ETH_LOCAL_MAC0; buf[7] = ETH_LOCAL_MAC1;
    buf[8] = ETH_LOCAL_MAC2; buf[9] = ETH_LOCAL_MAC3;
    buf[10] = ETH_LOCAL_MAC4; buf[11] = ETH_LOCAL_MAC5;
    buf[12] = 0x08; buf[13] = 0x00;
    buf[14] = 0x45; buf[17] = (u8)((20U + 8U + DHCP_PAYLOAD) >> 8);
    buf[18] = (u8)(20U + 8U + DHCP_PAYLOAD);
    buf[22] = 64; buf[23] = 17;
    dput_be16(buf + 34, DHCP_CLIENT_PORT);
    dput_be16(buf + 36, DHCP_SERVER_PORT);
    dput_be16(buf + 38, (u16)(8U + DHCP_PAYLOAD));
    {
        u8 *o = buf + 42;
        *o++ = DHCP_OP_REQUEST; *o++ = 1U; *o++ = 6U; *o++ = 0U;
        dput_be16(o, (u16)(dhcp_xid >> 16)); o += 2; dput_be16(o, (u16)dhcp_xid); o += 2;
        o += 4;                                  /* secs/flags=广播 */
        o += 8;                                  /* ciaddr=0 */
        o += 16;                                 /* yiaddr/siaddr=0 */
        memcpy(o, buf + 6, 6U); o += 10;         /* chaddr + 10 填充 */
        o += 64 + 128;                           /* sname/file */
        *o++ = (u8)(DHCP_MAGIC >> 24); *o++ = (u8)(DHCP_MAGIC >> 16);
        *o++ = (u8)(DHCP_MAGIC >> 8); *o++ = (u8)DHCP_MAGIC;
    }
    return buf;
}

static void dhcp_send(u8 msgtype, const u8 *reqip, const u8 *sid)
{
    u8 *buf = dhcp_frame();
    u8 *o = buf + 42U + 236U;
    u16 sum;
    if (!buf) return;
    /* xID */
    buf[46] = (u8)(dhcp_xid >> 24); buf[47] = (u8)(dhcp_xid >> 16);
    buf[48] = (u8)(dhcp_xid >> 8); buf[49] = (u8)dhcp_xid;
    *o++ = DHCP_OPT_MSGTYPE; *o++ = 1U; *o++ = msgtype;
    if (reqip) { *o++ = DHCP_OPT_REQIP; *o++ = 4U; memcpy(o, reqip, 4U); o += 4U; }
    if (sid)   { *o++ = DHCP_OPT_SERVERID; *o++ = 4U; memcpy(o, sid, 4U); o += 4U; }
    *o++ = DHCP_OPT_END;
    sum = dip_checksum(buf + 14, 20U);
    dput_be16(buf + 24, sum);
    sum = 0x0011U + (u16)(8U + DHCP_PAYLOAD);
    sum += (u16)~dip_checksum(buf + 26, 8U) & 0xFFFFU;
    sum += (u16)~dip_checksum(buf + 34, 8U + DHCP_PAYLOAD) & 0xFFFFU;
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    dput_be16(buf + 40, (u16)(sum == 0xFFFFU ? 0U : ~sum & 0xFFFFU));
    (void)eth_tx_send(buf, (u16)(14U + 20U + 8U + DHCP_PAYLOAD));
}

static void dhcp_apply(const u8 *ip, const u8 *mask, const u8 *gw)
{
    udp_stack_set_addresses(ip, mask, gw);
}

void udp_dhcp_start(void)
{
    dhcp_state = 1U; dhcp_attempts = DHCP_ATTEMPTS; dhcp_ticks = 0U;
    dhcp_xid = 0x50413331U;
    memset(dhcp_server, 0, 4U); memset(dhcp_offer_ip, 0, 4U);
    xil_printf("[dhcp] discover; fallback=%u.%u.%u.%u\r\n",
               ETH_IP_ADDR0, ETH_IP_ADDR1, ETH_IP_ADDR2, ETH_IP_ADDR3);
}

void udp_dhcp_rx(const u8 *pl, u32 plen)
{
    u8 msgtype = 0U;
    const u8 *opt, *end;
    if (plen < 240U) return;
    if (((u32)pl[236] << 24 | (u32)pl[237] << 16 | (u32)pl[238] << 8 | pl[239]) != DHCP_MAGIC) return;
    if (pl[4] != DHCP_OP_REPLY) return;
    if (((u32)pl[8] << 24 | (u32)pl[9] << 16 | (u32)pl[10] << 8 | pl[11]) != dhcp_xid) return;
    opt = pl + 240U; end = pl + plen;
    while (opt < end) {
        u8 t = *opt++;
        u8 l = 0;
        if (t == DHCP_OPT_END) break;
        if (t == 0U) continue;
        if (opt >= end) break;
        l = *opt++;
        if (opt + l > end) return;
        if (t == DHCP_OPT_MSGTYPE) msgtype = l ? opt[0] : 0U;
        if (t == DHCP_OPT_SERVERID && l == 4U) memcpy(dhcp_server, opt, 4U);
        opt += l;
    }
    if (msgtype == DHCP_MSG_OFFER && dhcp_state == 1U) {
        memcpy(dhcp_offer_ip, pl + 16, 4U);
        dhcp_state = 2U; dhcp_ticks = 0U;
        dhcp_send(DHCP_MSG_REQUEST, dhcp_offer_ip, dhcp_server);
        xil_printf("[dhcp] offer %u.%u.%u.%u; requesting\r\n",
                   pl[16], pl[17], pl[18], pl[19]);
        return;
    }
    if (msgtype == DHCP_MSG_ACK && dhcp_state == 2U) {
        u8 mask[4] = {255,255,255,0}, gw[4] = {0,0,0,0};
        opt = pl + 240U; end = pl + plen;
        while (opt < end) {
            u8 t = *opt++;
            u8 l = 0;
            if (t == DHCP_OPT_END) break;
            if (t == 0U) continue;
            if (opt >= end) break;
            l = *opt++;
            if (t == DHCP_OPT_SUBNET && l == 4U) memcpy(mask, opt, 4U);
            if (t == DHCP_OPT_ROUTER && l >= 4U) memcpy(gw, opt, 4U);
            opt += l;
        }
        dhcp_apply(pl + 16, mask, gw);
        dhcp_state = 3U;
        xil_printf("[dhcp] lease %u.%u.%u.%u acquired\r\n",
                   pl[16], pl[17], pl[18], pl[19]);
    }
}

void udp_dhcp_poll(void)
{
    if (dhcp_state == 3U || dhcp_state == 4U) return;
    if (!eth_link_is_up()) return;
    if (++dhcp_ticks < 30U) return;              /* 30 x 100ms */
    dhcp_ticks = 0U;
    if (dhcp_state == 2U && (dhcp_offer_ip[0]|dhcp_offer_ip[1]|dhcp_offer_ip[2]|dhcp_offer_ip[3])) {
        u8 mask[4] = {255,255,255,0}, gw[4] = {0,0,0,0};
        dhcp_apply(dhcp_offer_ip, mask, gw);
        dhcp_state = 3U;
        xil_printf("[dhcp] using offered address\r\n");
        return;
    }
    if (dhcp_attempts == 0U) {
        u8 fb[4] = {ETH_IP_ADDR0, ETH_IP_ADDR1, ETH_IP_ADDR2, ETH_IP_ADDR3};
        u8 mask[4] = {255,255,255,0}, gw[4] = {0,0,0,0};
        dhcp_apply(fb, mask, gw);
        dhcp_state = 4U;
        xil_printf("[dhcp] no server; static fallback\r\n");
        return;
    }
    --dhcp_attempts;
    dhcp_send(DHCP_MSG_DISCOVER, 0, 0);
}

u8 udp_dhcp_done(void) { return (dhcp_state == 3U || dhcp_state == 4U) ? 1U : 0U; }
