/* ============================================================================
 * eth_link.h —— P351 GEM0 + RTL8211F建链 + BD-ring发送引擎（UDP回传用）
 *
 * 背景：板载PHY RTL8211F硬接PS GEM0（RGMII MIO16-27、MDIO MIO52-53），
 *       PL fabric 无法访问 → UDP 只能走 PS 侧 emacps 驱动。本模块负责：
 *       - GEM0初始化 + MDIO扫描确认RTL8211F（UID 0x001CC916）
 *       - 配置F型号RGMII时延，自动协商100M全双工
 *       - TX/RX BD-ring 建立 + 中断挂接（GIC SPI 54，复用 dma_rx 的 GIC 实例）
 *       - 非阻塞 eth_tx_send()（BD 可分配则入队，否则丢弃返回非零）
 *
 * ⚠️ emacps 驱动不做 cache 维护（IS_CACHE_COHERENT=0 且 DCache 开）——
 *    必须手动 flush/invalidate BD 字与缓冲，这是本模块正确性的核心。
 *
 * 依赖：emacps_v3_12（BSP 已有），XPAR_XEMACPS_0_* 宏。
 *       中断 ID 硬编码 GEM0 SPI 54（XPAR_XEMACPS_0_INTR 不在 xparameters.h）。
 * ==========================================================================*/
#ifndef ETH_LINK_H
#define ETH_LINK_H

#include "xil_types.h"

/* ---- GEM0 中断（GIC SPI 54 = XPS_GEM0_INT_ID，不在 xparameters.h） ---- */
#define ETH_GEM_INTR_ID       54U

/* ---- 本机 MAC（可改） ---- */
#define ETH_LOCAL_MAC0  0x00
#define ETH_LOCAL_MAC1  0x0A
#define ETH_LOCAL_MAC2  0x35
#define ETH_LOCAL_MAC3  0x00
#define ETH_LOCAL_MAC4  0x00
#define ETH_LOCAL_MAC5  0x02

/* ---- 环/缓冲规模（26 帧/次采集，BD 与缓冲需 ≥ 帧数保证不丢） ---- */
#define TX_BD_CNT         32U
#define RX_BD_CNT         64U   /* 吸收Windows广播及UI切换时的控制包突发 */
#define TX_FRAME_BUFS     64U   /* CPU staging only; DMA has one private buffer per BD */
#define ETH_BUF_SIZE    1536U   /* 1500 MTU + 14 以太头 + 余量 */

/* ---- MDIO 扫描 ---- */
#define ETH_PHY_NONE    0xFFFFu
#define ETH_PHY_RTL_ID1 0x001Cu  /* Realtek OUI（ID1） */
#define ETH_PHY_RTL_ID2 0xC916u  /* P351 RTL8211F，禁止写RTL8211E的扩展页。 */

/* ---- API ---- */
int  eth_link_init(void);        /* 完整建链：GEM0+PHY+ring+中断；可重复调用（幂等） */
void eth_diag_poll(void);        /* 主循环轮询 GEM 错误位（ISR 被驱动入口清过，这里抓持续断言源） */
/* 主循环入口：250ms节流检查PHY；断线不解析队列，重连刷新GEM环后再处理新帧。
 * 每次最多一次MDIO、单次最多等待1ms；不调用数秒的启动协商，RF/BIT状态不因重连而改变。 */
void eth_rx_poll(void);
int eth_network_reset(void);     /* Main only: rebuild GEM rings, preserve RF/BIT/PHY. */
void eth_phy_cycle_rgmii_delay(void);  /* 'e' 键：循环 RGMII 时延 0/只RX/只TX/双开 */
void eth_loopback_test(void);          /* 'q' 键：GEM TX→RX 内部环回（二分 GEM vs PHY） */
void eth_loopback_off(void);           /* 'Q' 键：关回环回正常收发 */
void eth_phy_loopback_test(void);      /* 'h' 键：PHY 内部回环（BMCR bit14，二分 GEM↔PHY RGMII vs 铜线/PC） */
void eth_diag_dump_ring(void);         /* 'y' 键：dump 全部 RX BD w0(地址|wrap|new)+w1(状态) */
int  eth_phy_scan(void);         /* MDIO 扫描 0..31，返回 PHY 地址；无则 ETH_PHY_NONE */
int  eth_phy_setup(u8 addr);     /* 复位+F时延回读+100FD协商；返回0成功 */
u8   eth_link_is_up(void);       /* 当前链路状态 */
u8  *eth_tx_get_buf(void);       /* 轮转返回一个可写 TX 帧缓冲（填帧后交 eth_tx_send） */
int  eth_tx_send(const u8 *buf, u16 len);  /* 非阻塞；0=入队成功, 否则丢包 */
void eth_tx_reclaim(void);       /* 回收已完成 TX 的 BD（SendHandler 内调用） */
u32  eth_tx_pending(void);       /* 已入队但未回收的 TX BD */
u32  eth_tx_completed(void);     /* GEM TX 完成帧累计 */
u32  eth_tx_alloc_failures(void);/* TX BD 满导致的发送失败累计 */
u32  eth_tx_counter_faults(void);/* pending/ISR 计数不变量异常 */
u32  eth_rx_buffer_errors(void); /* RXSR bit0: buffer not available */
u32  eth_rx_overrun_errors(void);/* RXSR bit2: receive overrun */
u32  eth_rx_hw_owned(void);      /* 当前交给GEM的RX BD数 */
u32  eth_rx_sw_drops(void);      /* ISR->主循环软件队列满丢帧数 */
u32  eth_rx_queue_depth(void);   /* 待主循环解析的 RX 帧数 */
u32  eth_rx_recoveries(void);    /* BUFFNA/RXOVR 恢复次数 */
u32  eth_mac_error_count(void);  /* MAC error IRQ count; never print from ISR */
u32  eth_mac_last_error(void);   /* Last MAC error: direction[31:24] | code[23:0] */
u32  eth_rx_irq_faults(void);    /* RX ISR reentry or BD ring operation failure */
int  eth_link_deinit(void);      /* 停 DMA+关中断（一般不用） */

#endif /* ETH_LINK_H */
