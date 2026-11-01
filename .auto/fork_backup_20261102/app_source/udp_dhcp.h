/* 自研栈上的轻量 DHCP 客户端（RFC2131 子集，广播收发）。
 * DISCOVER→OFFER→REQUEST→ACK；成功写 board_ip/mask/gateway；
 * 3 轮失败回退编译期静态地址（ETH_IP_ADDR_*，当前=192.168.1.2）。
 * 无续租：板卡长在线场景由 UDP 网络重置流程重建租约（udp_dhcp_restart）。 */
#ifndef UDP_DHCP_H_
#define UDP_DHCP_H_
#include "xil_types.h"

void udp_dhcp_start(void);   /* udp_net_init 末尾调用一次 */
void udp_dhcp_rx(const u8 *pl, u32 plen);  /* udp_stack 收到端口68载荷时转发 */
void udp_dhcp_poll(void);    /* 主循环服务节拍内调用 */
u8   udp_dhcp_done(void);    /* 1=流程结束（获得租约或回退静态） */
#endif
