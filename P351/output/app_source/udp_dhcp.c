/* 轻量 DHCP 客户端：DISCOVER→OFFER→REQUEST→ACK，全广播；租约过半续租，
 * 续租失败进入 REBINDING，租约到期回退链路本地地址并继续周期性重试。
 * 只使用 eth_link 帧接口与 udp_stack 地址接口，不依赖 lwIP。 */
#include "udp_dhcp.h"
#include "udp_stack.h"
#include "eth_link.h"
#include "xtime_l.h"
#include "xil_printf.h"
#include <string.h>

#define DHCP_CLIENT_PORT   68U
#define DHCP_SERVER_PORT   67U
#define DHCP_PAYLOAD       300U
#define DHCP_ATTEMPTS      4U
#define DHCP_RETRY_SEC     3U
#define DHCP_FALLBACK_SEC  60U
#define DHCP_MAGIC         0x63825363UL

#define DHCP_OP_REQUEST    1U
#define DHCP_OP_REPLY      2U
#define DHCP_MSG_DISCOVER  1U
#define DHCP_MSG_OFFER     2U
#define DHCP_MSG_REQUEST   3U
#define DHCP_MSG_DECLINE   4U
#define DHCP_MSG_ACK       5U
#define DHCP_MSG_NAK       6U

#define DHCP_OPT_SUBNET    1U
#define DHCP_OPT_ROUTER    3U
#define DHCP_OPT_HOSTNAME  12U
#define DHCP_OPT_REQIP     50U
#define DHCP_OPT_LEASE     51U
#define DHCP_OPT_MSGTYPE   53U
#define DHCP_OPT_SERVERID  54U
#define DHCP_OPT_PARAMS    55U
#define DHCP_OPT_T1        58U
#define DHCP_OPT_T2        59U
#define DHCP_OPT_END       255U

enum {
    DHCP_OFF = 0,
    DHCP_INIT,
    DHCP_SELECTING,
    DHCP_REQUESTING,
    DHCP_BOUND,
    DHCP_RENEWING,
    DHCP_REBINDING,
    DHCP_FALLBACK,
    DHCP_SUSPENDED
};

static const u8 dhcp_mac[6] = { ETH_LOCAL_MAC0, ETH_LOCAL_MAC1, ETH_LOCAL_MAC2,
                                ETH_LOCAL_MAC3, ETH_LOCAL_MAC4, ETH_LOCAL_MAC5 };
static const u8 dhcp_fallback_ip[4]   = { 192U, 168U, 1U, 2U };
static const u8 dhcp_fallback_mask[4] = { 255U, 255U, 255U, 0U };
static const u8 dhcp_zero_gw[4]       = { 0U, 0U, 0U, 0U };

static u8  dhcp_state = DHCP_OFF;
static u8  dhcp_link_up;
static u8  dhcp_attempts;
static u32 dhcp_xid;
static u8  dhcp_offer[4];
static u8  dhcp_server[4];
static u32 dhcp_lease_sec, dhcp_t1_sec, dhcp_t2_sec;
static XTime dhcp_next, dhcp_t1_at, dhcp_t2_at, dhcp_lease_at;

static void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);  p[3] = (u8)v;
}
static u32 get_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u32 dhcp_next_xid(void)
{
    XTime now;
    XTime_GetTime(&now);
    dhcp_xid = (u32)now ^ ((u32)dhcp_mac[3] << 24) ^ ((u32)dhcp_mac[5] << 8);
    if (!dhcp_xid) dhcp_xid = 0x50443331UL;
    return dhcp_xid;
}

static void dhcp_send(u8 type, const u8 *requested_ip, const u8 *server_id,
                      u8 renewing)
{
    u8 *frame = eth_tx_get_buf();
    u8 *p;
    u8 *o;
    u32 total = 14U + 20U + 8U + DHCP_PAYLOAD;
    u32 udp_len = 8U + DHCP_PAYLOAD;
    u32 i;

    if (!frame) return;
    memset(frame, 0, total);
    memset(frame, 0xFF, 6U);
    for (i = 0U; i < 6U; ++i) frame[6U + i] = dhcp_mac[i];
    frame[12] = 0x08U; frame[13] = 0x00U;

    frame[14] = 0x45U;
    put_be16(frame + 16U, (u16)(20U + udp_len));
    put_be16(frame + 18U, 0U);
    put_be16(frame + 20U, 0U);
    frame[22] = 64U; frame[23] = 17U;
    if (renewing) {
        /* 续租请求携带 ciaddr；此时本机已拥有该地址。 */
        memcpy(frame + 26U, udp_stack_local_ip(), 4U);
    }
    memset(frame + 30U, 0xFF, 4U);              /* 255.255.255.255 */
    put_be16(frame + 24U, ip_checksum(frame + 14U, 20U));

    put_be16(frame + 34U, DHCP_CLIENT_PORT);
    put_be16(frame + 36U, DHCP_SERVER_PORT);
    put_be16(frame + 38U, (u16)udp_len);
    put_be16(frame + 40U, 0U);                  /* IPv4 允许 UDP 校验和为0 */

    p = frame + 42U;
    p[0] = DHCP_OP_REQUEST; p[1] = 1U; p[2] = 6U; p[3] = 0U;
    put_be32(p + 4U, dhcp_xid);
    put_be16(p + 8U, 0U);
    put_be16(p + 10U, 0x8000U);                 /* 广播应答 */
    if (renewing) memcpy(p + 12U, udp_stack_local_ip(), 4U); /* ciaddr */
    memcpy(p + 28U, dhcp_mac, 6U);
    put_be32(p + 236U, DHCP_MAGIC);

    o = p + 240U;
    *o++ = DHCP_OPT_MSGTYPE; *o++ = 1U; *o++ = type;
    if (requested_ip) {
        *o++ = DHCP_OPT_REQIP; *o++ = 4U; memcpy(o, requested_ip, 4U); o += 4U;
    }
    if (server_id) {
        *o++ = DHCP_OPT_SERVERID; *o++ = 4U; memcpy(o, server_id, 4U); o += 4U;
    }
    *o++ = DHCP_OPT_PARAMS; *o++ = 5U;
    *o++ = DHCP_OPT_SUBNET; *o++ = DHCP_OPT_ROUTER;
    *o++ = DHCP_OPT_LEASE; *o++ = DHCP_OPT_T1; *o++ = DHCP_OPT_T2;
    *o++ = DHCP_OPT_HOSTNAME; *o++ = 4U;
    *o++ = 'P'; *o++ = '3'; *o++ = '5'; *o++ = '1';
    *o++ = DHCP_OPT_END;

    (void)eth_tx_send(frame, (u16)total);
}

static void dhcp_apply(const u8 *ip, const u8 *mask, const u8 *gw)
{
    udp_stack_set_addresses(ip, mask, gw);
}

static void dhcp_set_lease(u32 lease, u32 t1, u32 t2)
{
    XTime now;
    if (lease < 30U) lease = 30U;
    if (lease > 86400UL) lease = 86400UL;
    if (!t1 || t1 >= lease) t1 = lease / 2U;
    if (!t2 || t2 <= t1 || t2 >= lease) t2 = lease - lease / 8U;
    if (t2 <= t1) t2 = t1 + 1U;
    dhcp_lease_sec = lease;
    dhcp_t1_sec = t1;
    dhcp_t2_sec = t2;
    XTime_GetTime(&now);
    dhcp_t1_at = now + (XTime)COUNTS_PER_SECOND * t1;
    dhcp_t2_at = now + (XTime)COUNTS_PER_SECOND * t2;
    dhcp_lease_at = now + (XTime)COUNTS_PER_SECOND * lease;
}

static void dhcp_begin_init(void)
{
    XTime now;
    dhcp_state = DHCP_INIT;
    dhcp_attempts = 0U;
    memset(dhcp_offer, 0, 4U);
    memset(dhcp_server, 0, 4U);
    XTime_GetTime(&now);
    dhcp_next = now;
}

static void dhcp_fallback(void)
{
    XTime now;
    dhcp_apply(dhcp_fallback_ip, dhcp_fallback_mask, dhcp_zero_gw);
    dhcp_state = DHCP_FALLBACK;
    XTime_GetTime(&now);
    dhcp_next = now + (XTime)COUNTS_PER_SECOND * DHCP_FALLBACK_SEC;
    xil_printf("[dhcp] no lease; fallback 192.168.1.2/24\r\n");
}

void udp_dhcp_start(void)
{
    dhcp_state = DHCP_INIT;
    dhcp_link_up = 0U;
    dhcp_attempts = 0U;
    dhcp_lease_sec = dhcp_t1_sec = dhcp_t2_sec = 0U;
    memset(dhcp_offer, 0, 4U);
    memset(dhcp_server, 0, 4U);
    dhcp_next = 0;
    (void)dhcp_next_xid();
    xil_printf("[dhcp] client started\r\n");
}

void udp_dhcp_suspend(void)
{
    if (dhcp_state != DHCP_SUSPENDED) {
        dhcp_state = DHCP_SUSPENDED;
        xil_printf("[dhcp] suspended after manual network config\r\n");
    }
}

u8 udp_dhcp_configured(void)
{
    return (dhcp_state == DHCP_BOUND || dhcp_state == DHCP_RENEWING ||
            dhcp_state == DHCP_REBINDING || dhcp_state == DHCP_FALLBACK) ? 1U : 0U;
}

void udp_dhcp_poll(void)
{
    XTime now;
    u8 up;
    if (dhcp_state == DHCP_OFF || dhcp_state == DHCP_SUSPENDED) return;
    XTime_GetTime(&now);
    up = eth_link_is_up();
    if (!up) {
        if (dhcp_link_up) {
            dhcp_state = DHCP_INIT;
            dhcp_attempts = 0U;
            dhcp_next = now;
        }
        dhcp_link_up = 0U;
        return;
    }
    if (!dhcp_link_up) {
        dhcp_link_up = 1U;
        dhcp_state = DHCP_INIT;
        dhcp_attempts = 0U;
        dhcp_next = now;
    }

    switch (dhcp_state) {
    case DHCP_INIT:
        if (now < dhcp_next) return;
        if (dhcp_attempts >= DHCP_ATTEMPTS) {
            dhcp_fallback();
            return;
        }
        (void)dhcp_next_xid();
        ++dhcp_attempts;
        dhcp_send(DHCP_MSG_DISCOVER, 0, 0, 0U);
        dhcp_state = DHCP_SELECTING;
        dhcp_next = now + (XTime)COUNTS_PER_SECOND * DHCP_RETRY_SEC;
        break;
    case DHCP_SELECTING:
    case DHCP_REQUESTING:
        if (now >= dhcp_next) {
            dhcp_state = DHCP_INIT;
            dhcp_next = now;
        }
        break;
    case DHCP_BOUND:
        if (now >= dhcp_t1_at) {
            (void)dhcp_next_xid();
            dhcp_send(DHCP_MSG_REQUEST, 0, 0, 1U);
            dhcp_state = DHCP_RENEWING;
            dhcp_next = now + (XTime)COUNTS_PER_SECOND * DHCP_RETRY_SEC;
        }
        break;
    case DHCP_RENEWING:
        if (now >= dhcp_t2_at) {
            dhcp_send(DHCP_MSG_REQUEST, 0, 0, 1U);
            dhcp_state = DHCP_REBINDING;
            dhcp_next = now + (XTime)COUNTS_PER_SECOND * DHCP_RETRY_SEC;
        } else if (now >= dhcp_next) {
            dhcp_send(DHCP_MSG_REQUEST, 0, 0, 1U);
            dhcp_next = now + (XTime)COUNTS_PER_SECOND * DHCP_RETRY_SEC;
        }
        break;
    case DHCP_REBINDING:
        if (now >= dhcp_lease_at) {
            dhcp_fallback();
        } else if (now >= dhcp_next) {
            dhcp_send(DHCP_MSG_REQUEST, 0, 0, 1U);
            dhcp_next = now + (XTime)COUNTS_PER_SECOND * DHCP_RETRY_SEC;
        }
        break;
    case DHCP_FALLBACK:
        if (now >= dhcp_next) dhcp_begin_init();
        break;
    default:
        break;
    }
}

void udp_dhcp_rx(const u8 *payload, u32 length)
{
    const u8 *opt, *end;
    u8 msg = 0U;
    u8 subnet[4] = { 255U, 255U, 255U, 0U };
    u8 router[4] = { 0U, 0U, 0U, 0U };
    u8 server[4] = { 0U, 0U, 0U, 0U };
    u8 offered[4] = { 0U, 0U, 0U, 0U };
    u32 lease = 0U, t1 = 0U, t2 = 0U;

    if (!payload || length < 240U) return;
    if (payload[0] != DHCP_OP_REPLY) return;
    if (get_be32(payload + 4U) != dhcp_xid) return;
    if (memcmp(payload + 28U, dhcp_mac, 6U) != 0) return;
    if (get_be32(payload + 236U) != DHCP_MAGIC) return;
    memcpy(offered, payload + 16U, 4U);          /* yiaddr */
    if (!(offered[0] | offered[1] | offered[2] | offered[3]))
        memcpy(offered, payload + 12U, 4U);      /* renewal ACK 的 ciaddr */

    opt = payload + 240U;
    end = payload + length;
    while (opt < end) {
        u8 tag = *opt++;
        u8 len;
        if (tag == DHCP_OPT_END) break;
        if (tag == 0U) continue;
        if (opt >= end) break;
        len = *opt++;
        if ((u32)(end - opt) < len) break;
        if (tag == DHCP_OPT_MSGTYPE && len == 1U) msg = opt[0];
        else if (tag == DHCP_OPT_SUBNET && len == 4U) memcpy(subnet, opt, 4U);
        else if (tag == DHCP_OPT_ROUTER && len >= 4U) memcpy(router, opt, 4U);
        else if (tag == DHCP_OPT_SERVERID && len == 4U) memcpy(server, opt, 4U);
        else if (tag == DHCP_OPT_LEASE && len == 4U) lease = get_be32(opt);
        else if (tag == DHCP_OPT_T1 && len == 4U) t1 = get_be32(opt);
        else if (tag == DHCP_OPT_T2 && len == 4U) t2 = get_be32(opt);
        opt += len;
    }

    if (msg == DHCP_MSG_OFFER && dhcp_state == DHCP_SELECTING) {
        memcpy(dhcp_offer, offered, 4U);
        memcpy(dhcp_server, server, 4U);
        dhcp_send(DHCP_MSG_REQUEST, dhcp_offer, dhcp_server, 0U);
        dhcp_state = DHCP_REQUESTING;
        XTime_GetTime(&dhcp_next);
        dhcp_next += (XTime)COUNTS_PER_SECOND * DHCP_RETRY_SEC;
        xil_printf("[dhcp] offer; requesting\r\n");
        return;
    }
    if (msg == DHCP_MSG_ACK &&
        (dhcp_state == DHCP_REQUESTING || dhcp_state == DHCP_RENEWING ||
         dhcp_state == DHCP_REBINDING)) {
        if (!(offered[0] | offered[1] | offered[2] | offered[3])) return;
        dhcp_apply(offered, subnet, router);
        dhcp_set_lease(lease ? lease : 3600U, t1, t2);
        dhcp_state = DHCP_BOUND;
        xil_printf("[dhcp] lease %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u\r\n",
                   offered[0], offered[1], offered[2], offered[3],
                   subnet[0], subnet[1], subnet[2], subnet[3],
                   router[0], router[1], router[2], router[3]);
        return;
    }
    if (msg == DHCP_MSG_NAK &&
        (dhcp_state == DHCP_REQUESTING || dhcp_state == DHCP_RENEWING ||
         dhcp_state == DHCP_REBINDING)) {
        xil_printf("[dhcp] NAK; restart discover\r\n");
        dhcp_begin_init();
    }
}
