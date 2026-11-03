/* 手写 ARP/IPv4/UDP 栈上的轻量 DHCP 客户端（RFC2131 子集）。
 * DISCOVER/REQUEST 全广播，支持续租（RENEWING/REBINDING）；
 * 60 s 无服务器时回退 192.168.1.2/24（当前 PC 有线网段，PC 无需静态 IP）；
 * udp_stack 上电初始地址仍是链路本地 169.254.1.2/16。
 * NCIP 手工改址后 suspend，避免 DHCP 覆盖。 */
#ifndef UDP_DHCP_H
#define UDP_DHCP_H

#include "xil_types.h"

void udp_dhcp_start(void);                 /* 网络初始化后调用一次 */
void udp_dhcp_poll(void);                  /* 主循环节拍内调用（非阻塞） */
void udp_dhcp_rx(const u8 *payload, u32 length); /* udp_stack 收到目的端口68时转发 */
void udp_dhcp_suspend(void);               /* NCIP 手工改址：停止 DHCP 覆盖 */
u8   udp_dhcp_configured(void);            /* 1=已有租约或已回退链路本地地址 */

#endif /* UDP_DHCP_H */
