/* ============================================================================
 * eth_link.c —— P351 GEM0 + RTL8211F 建链 + BD-ring 发送引擎
 *
 * 参考 emacps_v3_12 BD-ring 模型（无 XEmacPs_Send，用 BdRing* 系列）。
 * 关键点：
 *  1) 驱动不做 cache 维护 → TX 缓冲+BD 字发送前 flush，RX 缓冲+ring 读前 invalidate。
 *  2) 本阶段只广告100M全双工；协商成功后显式匹配GEM时钟与速率。
 *  3) XEmacPs_Start() 断言两个 ring 的 BaseBdAddr 非零 → 必须先建 ring。
 *  4) GIC SPI 54 复用 dma_rx 的 XScuGic 实例（dma_rx_gic_instance()），
 *     不二次 CfgInitialize，避免冲掉 DMA 的 IOC 中断。
 *
 * MDIO扫描只接受RTL8211F的0x001C/C916；禁止套用旧板E型号扩展页。
 * ==========================================================================*/
#include "eth_link.h"
#include "udp_stack.h"
#include "xemacps.h"
#include "xemacps_bd.h"
#include "xemacps_bdring.h"
#include "xemacps_hw.h"
#include "xparameters.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "xil_io.h"
#include "xscugic.h"
#include "sleep.h"
#include "xtime_l.h"
#include "xpseudo_asm.h"     /* mfcpsr/mtcpsr：CPSR save/restore 临界区原语 */

/* ---- dma_rx 的 GIC 实例 getter（本文件需要复用） ---- */
extern XScuGic *dma_rx_gic_instance(void);

/* ---- PHY寄存器：标准基本页 + RTL8211F私有页 ---- */
#define PHY_REG_BMCR     0x00   /* 基本控制 */
#define PHY_REG_BMSR     0x01   /* 基本状态 */
#define PHY_REG_ID1      0x02
#define PHY_REG_ID2      0x03
#define PHY_REG_GBCR     0x09   /* 1000BASE-T控制：bit9=广告1000FD */
#define PHY_REG_ANAR     0x04   /* AN 广告（bit8=100FD, bit6=10FD） */
#define PHY_REG_ANLPAR   0x05   /* AN 链路伙伴能力 */
#define PHY_REG_GBCSR    0x0A   /* 1000BASE-T 状态（bit11=对方 1000FD） */
#define PHY_REG_EXT      0x1F   /* RTL8211F页选择寄存器 */
#define PHY_BMCR_SOFTRST   0x8000
#define PHY_BMCR_FULLDUP   0x0100   /* bit8 全双工 */
#define PHY_BMCR_ANEN      0x1000   /* bit12 AN 使能 */
#define PHY_BMCR_RESTARTAN 0x0200   /* bit9 重启 AN */
#define PHY_ANAR_1000FD_ADV 0x01E1  /* 100FD/HD + 10FD/HD + IEEE802.3 selector */
#define PHY_BMCR_FORCE100FD 0x2100  /* 强制 100M 全双工：bit13=100M, bit8=FD, AN 关 */
#define PHY_BMSR_LINK     0x0004   /* bit2 link status（latch-low，需读两次） */

/* ---- CPSR 临界区（save/restore） ----
 * Xil_DisableInterrupts/EnableInterrupts 只清/置 I 位、不保存原状态；在 ISR（异常入口
 * 已置 I=1）内调 EnableInterrupts 会误清 I 位打开 IRQ 嵌套。这里用 mfcpsr/mtcpsr 做
 * 完整 save/restore，恢复调用点原 CPSR：主循环关 IRQ、ISR 内保持关，两处都安全。 */
#define ETH_CPSR_I_BIT   0x80u   /* CPSR bit7=I：置 1 = 关 IRQ（ARM 架构固定） */
static inline u32 eth_irq_save(void)
{
    u32 flags = mfcpsr();
    mtcpsr(flags | ETH_CPSR_I_BIT);   /* 置 I 位 = 关 IRQ */
    return flags;
}
static inline void eth_irq_restore(u32 flags)
{
    mtcpsr(flags);                    /* 恢复原 CPSR */
}

/* ---- 静态状态 ---- */
static XEmacPs Emac;
/* GEM walks packed BDs, so padding each BD to a cache line is NOT legal.
 * Reserve a whole MMU section for uncached descriptors instead: flushing one
 * cached BD must never write back stale completion bits of its neighbours. */
#include "xil_mmu.h"
static struct {
    XEmacPs_Bd tx[TX_BD_CNT];
    XEmacPs_Bd rx[RX_BD_CNT];
    u8 reserved[0x100000U - (TX_BD_CNT + RX_BD_CNT) * sizeof(XEmacPs_Bd)];
} bd_memory __attribute__((aligned(0x100000)));
#define tx_bd_ring bd_memory.tx
#define rx_bd_ring bd_memory.rx
static u8 tx_fbuf[TX_FRAME_BUFS][ETH_BUF_SIZE] __attribute__((aligned(64)));
/* Each hardware descriptor owns its payload until the BD is reclaimed.
 * tx_fbuf is staging only; failed submissions cannot overwrite DMA data. */
static u8 tx_dma_payload[TX_BD_CNT][ETH_BUF_SIZE] __attribute__((aligned(64)));
static u8 rx_fbuf[RX_BD_CNT][ETH_BUF_SIZE] __attribute__((aligned(64)));
/* RX ISR 只做“DMA buffer -> 软件队列”快速搬运，立即归还 BD。
 * UDP/ARP/控制协议解析放到主循环，避免在 IRQ 中发送应答、写寄存器
 * 或下发 BIT 时占住全部 RX BD，这是长时间运行后 BUFFNA 卡死的根因。
 * 队列深度为 2 的幂，实际可用 63 帧；DDR 增量约 96 KiB。 */
#define RX_SW_QUEUE_CNT     64U
#define RX_SW_QUEUE_MASK    (RX_SW_QUEUE_CNT - 1U)
#define RX_POLL_BUDGET       1U /* Execute latched commands before parsing the next packet. */
static u8  rx_sw_fbuf[RX_SW_QUEUE_CNT][ETH_BUF_SIZE] __attribute__((aligned(64)));
static u16 rx_sw_len[RX_SW_QUEUE_CNT];
static volatile u32 rx_sw_head = 0;
static volatile u32 rx_sw_tail = 0;
static u8 eth_local_mac[6] = { ETH_LOCAL_MAC0, ETH_LOCAL_MAC1, ETH_LOCAL_MAC2,
                               ETH_LOCAL_MAC3, ETH_LOCAL_MAC4, ETH_LOCAL_MAC5 };
static u8  eth_phy_addr   = 0xFF;      /* 未定 */
static volatile u8 eth_link_up_flag = 0;
static u8  eth_initialized = 0;
static u8 eth_phy_configured;
static XTime eth_link_poll_at;
static u8 eth_link_phase;
static u16 eth_link_values[7];
static volatile u32 tx_ring_buffers_in_hw = 0;  /* 入队但未回收的帧数 */
static u32 tx_fbuf_idx = 0;            /* eth_tx_get_buf 轮转索引 */
static volatile u32 eth_mac_err_cnt = 0;       /* ISR only records; main prints snapshots */
static volatile u32 eth_mac_err_last = 0;
static volatile u32 eth_rx_irq_fault_cnt = 0;
static volatile u32 eth_loopback_own_rx = 0;   /* 环回测试：收到本机 src MAC 的帧数 */
static volatile u32 eth_tx_done_cnt = 0;       /* GEM TX 完成中断计数（帧已出 MAC） */
static volatile u32 eth_tx_alloc_fail_cnt = 0; /* TX BD 环满，上层需重试 */
static volatile u32 eth_tx_counter_fault_cnt = 0; /* pending 计数竞态/损坏诊断 */
static volatile u32 eth_rx_buffna_cnt = 0;     /* RXSR BUFFNA：无可用RX BD */
static volatile u32 eth_rx_overrun_cnt = 0;    /* RXSR RXOVR */
static volatile u32 eth_rx_sw_drop_cnt = 0;    /* ISR->main 软件队列满 */
static volatile u32 eth_rx_recovery_cnt = 0;   /* BUFFNA/RXOVR 恢复次数 */

/* 前向声明 */
static void eth_tx_isr(void *CallBackRef);
static void eth_rx_isr(void *CallBackRef);
static void eth_err_isr(void *CallBackRef, u8 Direction, u32 ErrorCode);
static int eth_phy_force_rgmii_delay(u8 addr);

/* 单核 SPSC 队列：ISR 是唯一 producer，主循环是唯一 consumer。
 * 先写帧内容/长度，dmb 后再发布 head；队列满时只丢该网络帧，
 * RX BD 仍会立即归还硬件，因而不再把 GEM 卡死。 */
static void eth_rx_queue_push(const u8 *buf, u16 len)
{
    u32 head = rx_sw_head;
    u32 next = (head + 1U) & RX_SW_QUEUE_MASK;
    u32 i;

    if (next == rx_sw_tail) {
        eth_rx_sw_drop_cnt++;
        return;
    }
    for (i = 0; i < (u32)len; i++)
        rx_sw_fbuf[head][i] = buf[i];
    rx_sw_len[head] = len;
    dmb();
    rx_sw_head = next;
}

/* Clause22管理事务沿用BSP命令字，但用1ms总超时替代其无限等待。
 * 只在主循环调用；控制器忙时不覆盖正在进行的事务，失败不提交读值。 */
static int eth_mdio_transfer(u32 addr, u32 reg, u8 read, u16 *value)
{
    u32 command, base = Emac.Config.BaseAddress;
    XTime start, now;
    if (!value || addr > 31U || reg > 31U || read > 1U) return -1;
    if (!(XEmacPs_ReadReg(base, XEMACPS_NWSR_OFFSET) & XEMACPS_NWSR_MDIOIDLE_MASK))
        return -1;
    command = XEMACPS_PHYMNTNC_OP_MASK |
        (read ? XEMACPS_PHYMNTNC_OP_R_MASK : XEMACPS_PHYMNTNC_OP_W_MASK) |
        (addr << XEMACPS_PHYMNTNC_PHAD_SHFT_MSK) |
        (reg << XEMACPS_PHYMNTNC_PREG_SHFT_MSK) | (read ? 0U : *value);
    XTime_GetTime(&start);
    XEmacPs_WriteReg(base, XEMACPS_PHYMNTNC_OFFSET, command);
    do {
        if (XEmacPs_ReadReg(base, XEMACPS_NWSR_OFFSET) & XEMACPS_NWSR_MDIOIDLE_MASK) {
            if (read) *value = (u16)XEmacPs_ReadReg(base, XEMACPS_PHYMNTNC_OFFSET);
            return 0;
        }
        XTime_GetTime(&now);
    } while (now - start < (XTime)COUNTS_PER_SECOND / 1000U);
    return -1;
}

static int eth_phy_reg_read(u32 addr, u32 reg, u16 *val)
{
    return eth_mdio_transfer(addr, reg, 1U, val);
}

static int eth_phy_reg_write(u32 addr, u32 reg, u16 val)
{
    return eth_mdio_transfer(addr, reg, 0U, &val);
}

/* ==================== SLCR GEM0 参考时钟分频（官方 lwIP 例程 SetUpSLCRDivisors 移植） ====================
 * Zynq GEM 的参考时钟来自 SLCR ENET_CLK_CTRL(0xF8000140)：GEM_CLK = IO_PLL/(DIV0*DIV1)。
 * FSBL ps7_init.c 只置 GEM0_CPU_1XCLKACT，不写分频 → 分频器留在复位值(DIV0=DIV1=0)
 * → GEM_CLK 直接是 IO_PLL 高频 → RGMII 100M TX 时钟(应 25MHz)严重超速：
 *   TX 死（PHY 无法按 25MHz 采样 GTX_CLK）、RX 活（RGMII RX 用 PHY 提供的 RXC 采样）、
 *   MDIO 仍可读（时钟偏快但够用）——与本板"RX 收 PC 帧 + ping 不回"逐条吻合。
 * 官方 emacps lwIP 在 AN 后调 SetUpSLCRDivisors 显式写分频，本函数为等价移植：
 *   100M→DIV0=8 DIV1=5(/40)；1000M→DIV0=8 DIV1=1(/8)；10M→DIV0=8 DIV1=50(/400)。
 * 掩码 0xFC0FC0FF 清 DIV0[13:8]/DIV1[25:20] 保留其余；
 * 若 SRCSEL_EMIO(bit6)=1 则参考时钟来自 PL，跳过（本板 MIO RGMII，bit6=0 不跳过）。 */
static void eth_slcr_gem_clk(int speed)
{
    u32 v, w;
    u32 div0, div1;

    if (speed == 1000)      { div0 = 8; div1 = 1;  }
    else if (speed == 100)  { div0 = 8; div1 = 5;  }
    else                    { div0 = 8; div1 = 50; }   /* 10M */

    Xil_Out32(0xF8000008u, 0xDF0Du);        /* SLCR 解锁 */
    w = Xil_In32(0xF8000140u);
    if (w & (1u << 6)) {                    /* SRCSEL_EMIO：时钟来自 PL，无需分频 */
        Xil_Out32(0xF8000004u, 0x767Bu);      /* 所有退出路径均重新锁定SLCR */
        xil_printf("[eth] SLCR GEM0_CLK skip (EMIO src) 0x%08x\r\n", (unsigned)w);
        return;
    }
    v = (w & 0xFC0FC0FFu) | (div1 << 20) | (div0 << 8);
    Xil_Out32(0xF8000140u, v);
    Xil_Out32(0xF8000004u, 0x767Bu);        /* SLCR 上锁 */
    xil_printf("[eth] SLCR GEM0_CLK %uM: 0x%08x -> 0x%08x\r\n",
            speed, (unsigned)w, (unsigned)v);
}

/* ==================== MDIO扫描：找RTL8211F ==================== */
int eth_phy_scan(void)
{
    u16 id1, id2;
    int a;

    for (a = 0; a < 32; a++) {
        if (eth_phy_reg_read((u32)a, PHY_REG_ID1, &id1) != 0)
            continue;
        if (eth_phy_reg_read((u32)a, PHY_REG_ID2, &id2) != 0)
            continue;
        /* P351 RTL8211F: 0x001CC916。只对明确识别的型号写私有寄存器。 */
        if ((id1 == ETH_PHY_RTL_ID1) && (id2 == ETH_PHY_RTL_ID2)) {
            xil_printf("[eth] PHY at 0x%02x id=0x%04x%04x\r\n", a, (unsigned)id1, (unsigned)id2);
            eth_phy_addr = (u8)a;
            return (int)a;
        }
    }
    return (int)ETH_PHY_NONE;
}

/* ==================== 单地址尝试：AN 优先（双工一致是 TX 通的前提），失败回退强打 ====================
 * ⚠️ 强打 100M FD（AN 关）时 PC 无 FLP 可协商，只能并行检测 → 常判成 100M 半双工，
 *    板 FD / PC HD 双工不匹配 → RX 通、TX 帧全撞丢。故必须 AN 完成（BMSR bit5）才算建链。 */
static int eth_phy_try_link(u8 addr)
{
    u16 bmsr = 0, partner = 0;
    int i;
    eth_phy_configured = 0U;
    eth_link_up_flag = eth_link_phase = 0U;

    /* 软复位：BMCR bit15，写完等 ~20ms */
    if (eth_phy_reg_write(addr, PHY_REG_BMCR, PHY_BMCR_SOFTRST) != 0)
        return -1;
    for (i = 0; i < 100; ++i) {
        if (eth_phy_reg_read(addr, PHY_REG_BMCR, &bmsr) != 0)
            return -1;
        if (!(bmsr & PHY_BMCR_SOFTRST)) break;
        usleep(1000);
    }
    if (i == 100 || eth_phy_force_rgmii_delay(addr) != 0)
        return -1;
    bmsr = 0;

    /* 第一步：AN 协商。只广告 100BASE-T（不广告 1G，固定走 100M RGMII 档），
     * 等 link 且 AN-complete（BMSR bit5=0x0020）才确认建链。两步重启，~2.5s */
    if (eth_phy_reg_write(addr, PHY_REG_GBCR, 0x0000) != 0 ||
        eth_phy_reg_write(addr, PHY_REG_ANAR, 0x0101) != 0 || /* 仅100M全双工 */
        eth_phy_reg_write(addr, PHY_REG_BMCR, PHY_BMCR_FULLDUP | PHY_BMCR_ANEN) != 0)
        return -1;
    usleep(50000);
    if (eth_phy_reg_write(addr, PHY_REG_BMCR,
            PHY_BMCR_FULLDUP | PHY_BMCR_ANEN | PHY_BMCR_RESTARTAN) != 0)
        return -1;
    eth_phy_configured = 1U; /* 无网线也已完成配置，后插网线由运行期监督接管。 */
    for (i = 0; i < 100; i++) {
        if (eth_phy_reg_read(addr, PHY_REG_BMSR, &bmsr) != 0) return -1;
        usleep(5000);
        if (eth_phy_reg_read(addr, PHY_REG_BMSR, &bmsr) != 0) return -1;
        if ((bmsr & PHY_BMSR_LINK) && (bmsr & 0x0020u))   /* link + AN-complete */
            break;
        usleep(45000);
    }
    if ((bmsr & PHY_BMSR_LINK) && (bmsr & 0x0020u)) {
        /* 对端广告千兆不等于实际协商到千兆。本机只广告100FD。 */
        if (eth_phy_reg_read(addr, PHY_REG_ANLPAR, &partner) != 0 ||
            !(partner & 0x0100U)) return -1;
        eth_slcr_gem_clk(100);
        XEmacPs_SetOperatingSpeed(&Emac, 100);
        eth_link_up_flag = 1;
        xil_printf("[eth] link up 0x%02x (100FD AN, partner=0x%04x)\r\n",
                (unsigned)addr, (unsigned)partner);
        return 0;
    }

    /* P351不将强制全双工的并行检测当作可用链路，避免对端半双工。 */
    xil_printf("[eth] RTL8211F AN incomplete; link DOWN, local RF service continues\r\n");
    return -1;
#if 0 /* 旧板的强打诊断不进入P351生产链路。 */
    /* 第二步：AN 失败回退强打 100M FD（仅诊断；PC 并行检测可能落 HD，TX 可能不通） */
    eth_phy_reg_write(addr, PHY_REG_BMCR, PHY_BMCR_FORCE100FD);
    for (i = 0; i < 40; i++) {
        eth_phy_reg_read(addr, PHY_REG_BMSR, &bmsr);   /* 清 latch */
        usleep(5000);
        eth_phy_reg_read(addr, PHY_REG_BMSR, &bmsr);
        if (bmsr & PHY_BMSR_LINK)
            break;
        usleep(45000);
    }
    if (bmsr & PHY_BMSR_LINK) {
        eth_slcr_gem_clk(100);
        XEmacPs_SetOperatingSpeed(&Emac, 100);
        eth_link_up_flag = 1;
        xil_printf("[eth] link up 0x%02x (forced 100M FD, anCpl=%u)\r\n",
                (unsigned)addr, (unsigned)((bmsr >> 5) & 1u));
        return 0;
    }

    xil_printf("[eth] link DOWN after addr 0x%02x (AN + forced 100M both failed)\r\n",
            (unsigned)addr);
    return -1;
#endif
}

/* ==================== 仅在扫描确认的PHY地址协商100FD ==================== */
int eth_phy_setup(u8 addr)
{
    if (eth_phy_try_link(addr) == 0)
        return 0;
    xil_printf("[eth] link DOWN (identified RTL8211F at 0x%02x)\r\n", (unsigned)addr);
    eth_link_up_flag = 0;
    return -1;
}

/* P351厂家DTB为rgmii-id，按RTL8211F接口设置双方内部时延，必须回读并恢复页。 */
static int eth_phy_set_f_delay(u8 addr, u8 mode)
{
    u16 oldpage, tx, rx, check;
    int status = -1;
    /* 厂家DTB为rgmii-id。RTL8211F: page D08, TX reg11 bit8、RX reg15 bit3。
     * 保留无关位并回读；任何MDIO失败都不能宣称网口初始化成功。 */
    if (eth_phy_reg_read(addr, 0x1F, &oldpage) != 0) return -1;
    if (eth_phy_reg_write(addr, 0x1F, 0x0D08) != 0) goto restore;
    if (eth_phy_reg_read(addr, 0x11, &tx) != 0 ||
        eth_phy_reg_read(addr, 0x15, &rx) != 0) goto restore;
    tx = (u16)((tx & ~0x0100U) | ((mode & 2U) ? 0x0100U : 0U));
    rx = (u16)((rx & ~0x0008U) | ((mode & 1U) ? 0x0008U : 0U));
    if (eth_phy_reg_write(addr, 0x11, tx) != 0 ||
        eth_phy_reg_write(addr, 0x15, rx) != 0) goto restore;
    if (eth_phy_reg_read(addr, 0x11, &check) != 0 || check != tx) goto restore;
    if (eth_phy_reg_read(addr, 0x15, &check) != 0 || check != rx) goto restore;
    status = 0;
restore:
    if (eth_phy_reg_write(addr, 0x1F, oldpage) != 0) status = -1;
    return status;
}
static int eth_phy_force_rgmii_delay(u8 addr)
{
    int status = eth_phy_set_f_delay(addr, 3U);
    xil_printf("[eth] RTL8211F TX/RX 2ns delay %s\r\n", status ? "FAIL" : "verified");
    return status;
}

/* F型号实时状态在A43页12寄存器，不能读取E型号的基本页1A。 */
static int eth_phy_f_status(u8 addr, u16 *value)
{
    u16 oldpage;
    int status = -1;
    *value = 0xFFFFU;
    if (eth_phy_reg_read(addr, 0x1F, &oldpage) != 0) return -1;
    if (eth_phy_reg_write(addr, 0x1F, 0x0A43) == 0)
        status = eth_phy_reg_read(addr, 0x12, value);
    if (eth_phy_reg_write(addr, 0x1F, oldpage) != 0) status = -1;
    if (status != 0) *value = 0xFFFFU;
    return status;
}

/* ---- RGMII 时延循环切换（诊断）：'e' 键试 4 种组合 ----
 * 0=强制无时延  1=只RX  2=只TX  3=RX+TX（boot 默认）。 */
static u8 eth_rgmii_mode = 3;
void eth_phy_cycle_rgmii_delay(void)
{
    u8 addr = eth_phy_addr;
    u8 next_mode;
    if (addr == 0xFF) { xil_printf("[eth] no PHY, can't cycle delay\r\n"); return; }
    next_mode = (eth_rgmii_mode + 1U) & 3U;
    if (eth_phy_set_f_delay(addr, next_mode) != 0) {
        eth_link_up_flag = 0;
        xil_printf("[eth] RGMII mode write/readback FAIL\r\n");
        return;
    }
    eth_rgmii_mode = next_mode;
    xil_printf("[eth] RTL8211F RGMII mode=%u\r\n", (unsigned)eth_rgmii_mode);
}

/* ==================== GEM 内部回环（'q'）：二分 GEM/DMA/软件 vs PHY 数据 ====================
 * NWCTRL bit1 LOOPEN=1：TX 数据流在 MAC 内部直接喂回 RX 通路（不经 RGMII IO / 不经 PHY）。
 * 环回后发一帧已知 ARP 请求（广播，必被地址过滤接受）：
 *  - RX 收到与原发送完全一致的字节 → GEM TX 引擎 + RX DMA + BD 环 + cache + 解析全正常，
 *    问题 100% 在 PHY 的 RGMII RX 数据驱动（RXD0-3 线为低/开路）或板上走线。
 *  - RX 仍全零/乱码 → GEM RX 路径或软件 bug（与 PHY 无关）。
 * 'q' 开环回并发帧；'Q' 关回环回正常收发。 */
void eth_loopback_test(void)
{
    u8  *buf;
    u16 len;
    u32 reg;
    u32 i;

    if (!eth_initialized) {
        xil_printf("[eth] not init\r\n");
        return;
    }
    buf = eth_tx_get_buf();
    if (udp_build_arp_request(buf, &len) != 0) {
        xil_printf("[eth] loopback deferred: no controller/ARP peer\r\n");
        return; /* 尚未切换GEM模式，不能因未配对而遗留内部环回。 */
    }
    reg = XEmacPs_ReadReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET);
    XEmacPs_WriteReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET,
                    reg | XEMACPS_NWCTRL_LOOPEN_MASK);
    xil_printf("[eth] LOOPEN=1 (GEM internal loopback)\r\n");
    eth_loopback_own_rx = 0;
    eth_tx_done_cnt = 0;
    udp_stack_rx_reset();
    if (eth_tx_send(buf, len) != 0) {
        XEmacPs_WriteReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET, reg);
        xil_printf("[eth] loopback ARP send FAILED\r\n");
        return;
    }
    /* 等环回帧走完 GEM 内部回环、被 RX DMA 收下（回环 ~200ns，50ms 充足）。
     * 收不到 = GEM TX 引擎/软件侧仍死；收到 = GEM TX+SW 完好，故障锁在 PHY/RGMII 外路。 */
    for (i = 0; i < 50; i++) {
        usleep(1000);
        if (eth_loopback_own_rx)
            break;
    }
    if (eth_loopback_own_rx)
        xil_printf("[eth] loopback OK: own frame rx (GEM TX DMA+SW good), tx_done=%u\r\n",
                (unsigned)eth_tx_done_cnt);
    else
        xil_printf("[eth] loopback FAIL: no own frame rx (TX engine or SW), tx_done=%u isr=0x%08x\r\n",
                (unsigned)eth_tx_done_cnt,
                (unsigned)XEmacPs_ReadReg(Emac.Config.BaseAddress, XEMACPS_ISR_OFFSET));
}

void eth_loopback_off(void)
{
    u32 reg;
    if (!eth_initialized) return;
    reg = XEmacPs_ReadReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET);
    reg &= (u32)(~XEMACPS_NWCTRL_LOOPEN_MASK);
    XEmacPs_WriteReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET, reg);
    xil_printf("[eth] LOOPEN=0 (back to external)\r\n");
}

/* ==================== PHY 内部回环（'h'）：二分 GEM↔PHY RGMII 通路 vs 铜线/PC ====================
 * BMCR bit14(0x4000)=loopback：PHY把从RGMII收到的TX数据直接环回RGMII RX，
 * 完全不经铜线/变压器/PC。前提：GEM 内部回环(LOOPEN)已确认 OK（GEM TX 引擎+软件全好）。
 *  - PHY 环回 OK   → GEM↔PHY RGMII 双向全通 → 故障锁死在铜线/PC/链路速率（PHY 之外）；
 *  - PHY 环回 FAIL → RGMII TX 方向（GEM→PHY）在接口/PHY 端坏：RGMII TX 时钟(100M 靠 PHY 提供
 *    TXC)、TX 时延、或 RGMII 焊线。RX 方向已证好（PC 帧能进）。 */
/* 环回探测：下发一帧已知 ARP 广播，等本机 src MAC 帧回读；返回 1=回读成功 */
static u32 eth_loopback_probe(u32 *err_isr)
{
    u8  *buf;
    u16 len;
    u32 i;
    if (err_isr) *err_isr = 0U;

    buf = eth_tx_get_buf();
    if (udp_build_arp_request(buf, &len) != 0) {
        if (err_isr) *err_isr = 0U;
        return 0U;
    }
    if (eth_tx_send(buf, len) != 0)
        return 0;
    for (i = 0; i < 100; i++) {       /* 100ms 上限，回环 ~200ns */
        usleep(1000);
        if (eth_loopback_own_rx)
            break;
    }
    if (err_isr)
        *err_isr = XEmacPs_ReadReg(Emac.Config.BaseAddress, XEMACPS_ISR_OFFSET);
    return eth_loopback_own_rx ? 1 : 0;
}

/* ==================== PHY 内部回环（'h'）：二分 GEM↔PHY RGMII vs 铜线/PC ====================
 * BMCR bit14(0x4000)=PHY loopback：PHY 把从 RGMII 收到的 TX 数据环回 RGMII RX，不经铜线/PC。
 * 先 100M 强制、再 1000M 强制主——两种速度的 RGMII TX 时钟源不同（100M 由 GEM 内部分频自驱，
 * 1000M 由 GEM 出 125MHz GTXCLK），结果能分辨：
 *   100M FAIL + 1000M OK   → 100M 档 RGMII TX 时钟/数据路径坏（分频、时延、PHY 100M 采样）；
 *   100M FAIL + 1000M FAIL → RGMII TX 整条通路坏（GEM_CLK、焊线、GEM 配置）。
 * F状态页A43/12用于诊断；失败打印FFFF，不作为射频正确性的证据。
 * 恢复用软复位+重走 boot 建链（直接写 BMCR 恢复曾导致铜线不回）。 */
void eth_phy_loopback_test(void)
{
    u16 bmcr = 0xFFFFU, bmsr = 0xFFFFU, psr = 0xFFFFU;
    u32 reg, isr = 0U;
    u32 ok;

    if (!eth_initialized || eth_phy_addr == 0xFF) {
        xil_printf("[eth] not init\r\n");
        return;
    }
    /* 先关 GEM 内部回环（避免叠加），静默清 LOOPEN */
    reg = XEmacPs_ReadReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET);
    XEmacPs_WriteReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET,
            reg & (u32)(~XEMACPS_NWCTRL_LOOPEN_MASK));

    eth_phy_reg_read(eth_phy_addr, PHY_REG_BMCR, &bmcr);
    (void)eth_phy_f_status(eth_phy_addr, &psr);
    xil_printf("[eth] PHY pre-test BMCR=0x%04x PSR=0x%04x\r\n", (unsigned)bmcr, (unsigned)psr);

    /* ---- 1) 100M 强制回环（AN 关 + 强制 100M FD，BMCR bit14） ---- */
    eth_phy_reg_write(eth_phy_addr, PHY_REG_BMCR, PHY_BMCR_FORCE100FD | 0x4000u);
    usleep(20000);
    eth_phy_reg_read(eth_phy_addr, PHY_REG_BMSR, &bmsr);   /* 清 latch */
    eth_phy_reg_read(eth_phy_addr, PHY_REG_BMSR, &bmsr);
    (void)eth_phy_f_status(eth_phy_addr, &psr);
    eth_loopback_own_rx = 0;
    eth_tx_done_cnt = 0;
    udp_stack_rx_reset();
    ok = eth_loopback_probe(&isr);
    xil_printf("[eth] PHY lb 100M: BMSR=0x%04x PSR=0x%04x %s tx_done=%u isr=0x%08x\r\n",
            (unsigned)bmsr, (unsigned)psr, ok ? "OK" : "FAIL",
            (unsigned)eth_tx_done_cnt, (unsigned)isr);

    /* ---- 2) 1000M诊断：bit12使能主从手动配置、bit11主、bit9广告1000FD ---- */
    eth_phy_reg_write(eth_phy_addr, PHY_REG_GBCR, 0x1A00u);
    eth_phy_reg_write(eth_phy_addr, PHY_REG_ANAR, PHY_ANAR_1000FD_ADV);
    eth_phy_reg_write(eth_phy_addr, PHY_REG_BMCR,
            PHY_BMCR_FULLDUP | PHY_BMCR_ANEN | 0x4000u);
    usleep(100000);
    eth_phy_reg_read(eth_phy_addr, PHY_REG_BMSR, &bmsr);
    eth_phy_reg_read(eth_phy_addr, PHY_REG_BMSR, &bmsr);
    (void)eth_phy_f_status(eth_phy_addr, &psr);
    eth_loopback_own_rx = 0;
    eth_tx_done_cnt = 0;
    udp_stack_rx_reset();
    ok = eth_loopback_probe(&isr);
    xil_printf("[eth] PHY lb 1000M: BMSR=0x%04x PSR=0x%04x %s tx_done=%u isr=0x%08x\r\n",
            (unsigned)bmsr, (unsigned)psr, ok ? "OK" : "FAIL",
            (unsigned)eth_tx_done_cnt, (unsigned)isr);

    /* ---- 恢复：软复位 + 重走 boot 建链（确保铜线重新协商回来） ---- */
    eth_phy_setup(eth_phy_addr);
    eth_phy_force_rgmii_delay(eth_phy_addr);
    xil_printf("[eth] PHY loopback done, link=%u\r\n", (unsigned)eth_link_up_flag);
}

/* ==================== 主循环 GEM 错误位轮询（诊断） ====================
 * ISR 在驱动入口被清，ISR 里读常为 0。但若错误源持续断言（RX 溢出/缓冲耗尽
 * 等），清除后立即重新置位，主循环轮询能抓到。 */
void eth_diag_poll(void)
{
    /* 静默清除 GEM 错误位（持续断言源会立即重新置位），不打印 */
    u32 isr = XEmacPs_ReadReg(Emac.Config.BaseAddress, XEMACPS_ISR_OFFSET);
    if (isr & 0x0C70u)   /* HRESP 0x800 | RXOVR 0x400 | TXEXH 0x40 | RETRY 0x20 | URUN 0x10 */
        XEmacPs_WriteReg(Emac.Config.BaseAddress, XEMACPS_ISR_OFFSET, isr);
}

u8 eth_link_is_up(void) { return eth_link_up_flag; }

/* 每250ms检查一次，不调用启动时数秒的复位/协商等待。
 * 检测到拔线或MDIO故障后撤销软件UP；重连只恢复网络环，不改RF/BIT/屏幕。
 * 每次主循环只做一次MDIO事务，给RX位流/屏幕留出服务机会。
 * 已初始化但无网线的启动也会进入此处，避免始终停留在link=0。 */
static void eth_link_service(void)
{
    static const u8 registers[7] = {PHY_REG_BMSR, PHY_REG_BMSR, PHY_REG_BMCR,
        PHY_REG_ANAR, PHY_REG_ANLPAR, PHY_REG_GBCR, PHY_REG_BMSR};
    XTime now;
    if (!eth_initialized || !eth_phy_configured || eth_phy_addr == 0xFFU) return;
    if (eth_link_phase == 0U) {
        XTime_GetTime(&now);
        if (now - eth_link_poll_at < (XTime)COUNTS_PER_SECOND / 4U) return;
        eth_link_poll_at = now;
    }
    if (eth_phy_reg_read(eth_phy_addr, registers[eth_link_phase],
                         &eth_link_values[eth_link_phase]) != 0) goto down;
    switch (eth_link_phase) {
    case 0U:
        if (!(eth_link_values[0] & PHY_BMSR_LINK)) eth_link_up_flag = 0U;
        break; /* 第一次BMSR清除latch-low；末尾再次检查当前链路。 */
    case 1U: case 6U:
        if ((eth_link_values[eth_link_phase] & 0x0024U) != 0x0024U) goto down;
        break;
    case 2U:
        if (!(eth_link_values[2] & PHY_BMCR_ANEN) || (eth_link_values[2] & 0xCC00U)) goto down;
        break;
    case 3U:
        if ((eth_link_values[3] & 0x03E0U) != 0x0100U) goto down;
        break;
    case 4U:
        if (!(eth_link_values[4] & 0x0100U)) goto down;
        break;
    case 5U:
        if (eth_link_values[5] & 0x0300U) goto down;
        break;
    }
    if (++eth_link_phase < 7U) return;
    eth_link_phase = 0U;
    if (eth_link_up_flag) return;
    eth_link_up_flag = 0U;
    eth_slcr_gem_clk(100);
    XEmacPs_SetOperatingSpeed(&Emac, 100);
    /* 丢弃断线期间积压的网络帧/描述符，不能在重连后执行旧RF命令。 */
    if (eth_network_reset() != 0) return;
    eth_link_up_flag = 1U;
    xil_printf("[eth] link UP 100FD; network rings refreshed, RF/BIT unchanged\r\n");
    return;
down:
    eth_link_phase = 0U;
    if (eth_link_up_flag) xil_printf("[eth] link DOWN; local RF/BIT unchanged\r\n");
    eth_link_up_flag = 0U;
}

/* ==================== BD wrap 位（环尾标记） ====================
 * Zynq GEM 的 RX BD word0 低 2 位不是地址：bit0=NEW(占用)、bit1=WRAP(环尾)。
 * 地址在 bits31:2。DMA 靠 WRAP 位知道环尾并回绕 RXQBASE；未设则线性走出
 * 环外（把环后内存当描述符）→ RX 恰 16 帧后停、TX 无回传。必须给环最后
 * 一个 BD 置 wrap。 */
static void eth_bd_rx_wrap(XEmacPs_Bd *Bd)
{
    XEmacPs_BdWrite(Bd, XEMACPS_BD_ADDR_OFFSET,
            XEmacPs_BdRead(Bd, XEMACPS_BD_ADDR_OFFSET) | XEMACPS_RXBUF_WRAP_MASK);
}

static void eth_bd_tx_wrap(XEmacPs_Bd *Bd)
{
    XEmacPs_BdWrite(Bd, XEMACPS_BD_STAT_OFFSET,
            XEmacPs_BdRead(Bd, XEMACPS_BD_STAT_OFFSET) | XEMACPS_TXBUF_WRAP_MASK);
}

/* 'y' 键：dump 整个 RX 环 BD（w0=缓冲地址|wrap|new，w1=状态） */
void eth_diag_dump_ring(void)
{
    u32 i;
    if (!eth_initialized) { xil_printf("[eth] not init\r\n"); return; }
    for (i = 0; i < RX_BD_CNT; i++)
        xil_printf("[ring%02u] w0=0x%08x w1=0x%08x\r\n", i,
                (unsigned)XEmacPs_BdRead(&rx_bd_ring[i], XEMACPS_BD_ADDR_OFFSET),
                (unsigned)XEmacPs_BdRead(&rx_bd_ring[i], XEMACPS_BD_STAT_OFFSET));
}

/* ==================== TX 完成中断：回收 BD ==================== */
static void eth_tx_isr(void *CallBackRef)
{
    XEmacPs *p = (XEmacPs *)CallBackRef;
    XEmacPs_Bd *BdSet;
    u32 cnt;

    (void)p;
    Xil_DCacheInvalidateRange((UINTPTR)tx_bd_ring, sizeof(tx_bd_ring));
    cnt = XEmacPs_BdRingFromHwTx(&Emac.TxBdRing, TX_BD_CNT, &BdSet);
    if (cnt) {
        eth_tx_done_cnt += cnt;   /* 帧完成统计：DMA 已把帧送进 GEM TX MAC */
        /* 整环已在 FromHwTx 前 invalidate。BdSet 可跨越环尾，
         * 不能把它当成线性 cnt 数组再次 invalidate。 */
        XEmacPs_BdRingFree(&Emac.TxBdRing, cnt, BdSet);
        /* 统计：每次完成回填 1 个缓冲 */
        if (tx_ring_buffers_in_hw >= cnt) {
            tx_ring_buffers_in_hw -= cnt;
        } else {
            /* 正常实现中不可能出现。记录并自校正，避免计数
             * 下溢后让上层永久误判 TX 环已满。 */
            eth_tx_counter_fault_cnt++;
            tx_ring_buffers_in_hw = 0U;
        }
    }
}

/* ==================== RX 中断：快速搬运 + 分批归还 BD ==================== */
static void eth_rx_isr(void *CallBackRef)
{
    XEmacPs *p = (XEmacPs *)CallBackRef;
    XEmacPs_Bd *BdSet, *Bd;
    static u8  rx_busy = 0;      /* 重入保护：防中断风暴死锁主循环 */
    u32 cnt, i, len;

    (void)p;
    if (rx_busy) {               /* 理论上 IRQ 不嵌套；发生即风暴 */
        eth_rx_irq_fault_cnt++;
        return;
    }
    rx_busy = 1;

    Xil_DCacheInvalidateRange((UINTPTR)rx_bd_ring, sizeof(rx_bd_ring));
    cnt = XEmacPs_BdRingFromHwRx(&Emac.RxBdRing, RX_BD_CNT, &BdSet);
    if (cnt == 0) {
        /* XEmacPs_IntrHandler 在分发前已清除本次 RX 原因。这里绝不能
         * 再读取并清空整个 ISR：处理 RX 期间新到的 TXCOMPL 可能也
         * 在其中，误清会让 TX BD 永久少回收一次。 */
        rx_busy = 0;
        return;
    }
    /* 上面已 invalidate 整个环。BdSet 是逻辑环集合，可从
     * 末尾跨到开头，不是物理上连续的 cnt 个结构。 */
    Bd = BdSet;
    for (i = 0; i < cnt; i++) {
        len = (u32)XEmacPs_GetRxFrameSize(&Emac, Bd);
        if (len > 0 && len <= ETH_BUF_SIZE) {
            /* 关键修复（全零数据根因）：
             * XEmacPs_BdGetBufAddr 返回 word0 原始值（含 NEW 位 bit0），需掩掉低 2 位
             * 才是真缓冲基址；否则 buf=基址+1。且 Xil_DCacheInvalidateRange 对非 32B
             * 对齐的起/止会先 FLUSH(写回)该缓存行——若该行还存着 BSS 旧零，就会把零
             * 写回 DDR 覆盖 DMA 刚写入的帧 → 帧全零。修复=掩码取基址 + 起止对齐到
             * 32B 整行作 invalidate（Xilinx 文档明示：起止都必须行对齐才安全）。 */
            u32 base = XEmacPs_BdGetBufAddr(Bd) & XEMACPS_RXBUF_ADD_MASK;
            u32 inv_s = base & ~(u32)0x1F;
            u32 inv_e = (base + len + 31U) & ~(u32)0x1F;
            if (inv_e > base + ETH_BUF_SIZE)
                inv_e = base + ETH_BUF_SIZE;   /* 不越界到下一行（base+1536 仍 32B 对齐） */
            u8 *buf = (u8 *)(UINTPTR)base;
            Xil_DCacheInvalidateRange((UINTPTR)inv_s, inv_e - inv_s);
            /* 环回判定：src MAC == 本机 → 是 LOOPEN 回环帧（外部 PC 帧不会撞本机 MAC） */
            if (len >= 12U &&
                buf[6] == eth_local_mac[0] && buf[7] == eth_local_mac[1] &&
                buf[8] == eth_local_mac[2] && buf[9] == eth_local_mac[3] &&
                buf[10] == eth_local_mac[4] && buf[11] == eth_local_mac[5])
                eth_loopback_own_rx++;
            /* ISR 仅搬运。ARP/UDP/控制命令在 eth_rx_poll() 中解析，
             * 避免 TX 应答或 BIT 下发长时间占住 GEM IRQ。 */
            eth_rx_queue_push(buf, (u16)len);
        }
        Bd = XEmacPs_BdRingNext(&Emac.RxBdRing, Bd);
    }
    /* 归还 + 重新提交同一批 RX BD */
    if (XEmacPs_BdRingFree(&Emac.RxBdRing, cnt, BdSet) != XST_SUCCESS) {
        eth_rx_irq_fault_cnt++;
        rx_busy = 0;
        return;
    }
    if (XEmacPs_BdRingAlloc(&Emac.RxBdRing, cnt, &BdSet) != XST_SUCCESS) {
        eth_rx_irq_fault_cnt++;
        rx_busy = 0;
        return;
    }
    Bd = BdSet;
    for (i = 0; i < cnt; i++) {
        /* 用 BD 在环中的真实位置定位对应 fbuf（修复之前按局部 i 取 fbuf，
         * 批内 i 与环位置不一致 → 缓冲地址错乱/多 BD 共指同一 fbuf 覆盖）。
         * 环尾 BD 必须带 wrap 位，DMA 才会回绕环首继续收。 */
        u32 pos = (u32)(((UINTPTR)Bd - (UINTPTR)rx_bd_ring) / sizeof(XEmacPs_Bd));
        XEmacPs_BdSetAddressRx(Bd, (UINTPTR)&rx_fbuf[pos % RX_BD_CNT][0]);
        XEmacPs_BdClearRxNew(Bd);
        if (pos % RX_BD_CNT == RX_BD_CNT - 1)
            eth_bd_rx_wrap(Bd);
        Bd = XEmacPs_BdRingNext(&Emac.RxBdRing, Bd);
    }
    /* 可跨环尾，所以直接 flush 整个小型 RX 环。 */
    Xil_DCacheFlushRange((UINTPTR)rx_bd_ring, sizeof(rx_bd_ring));
    XEmacPs_BdRingToHw(&Emac.RxBdRing, cnt, BdSet);
    rx_busy = 0;
}

/* Parse only one packet per main-loop pass: STOP/CLEAR/TXCF must be applied
 * before the following BEGIN/START, even when UDP packets arrive in a burst. */
void eth_rx_poll(void)
{
    u32 budget = RX_POLL_BUDGET;
    static u32 poll_divider;
    eth_link_service();
    if (!eth_initialized || !eth_link_up_flag) return;
    /* Recover completions even if a GEM interrupt edge was lost. This never
     * parses commands with IRQs disabled; it only reclaims/copies descriptors. */
    if (eth_initialized && ((++poll_divider & 255U) == 0U)) {
        u32 flags = eth_irq_save();
        eth_tx_isr(&Emac);
        eth_rx_isr(&Emac);
        eth_irq_restore(flags);
    }
    while (budget--) {
        u32 tail = rx_sw_tail;
        u16 len;
        if (tail == rx_sw_head)
            break;
        dmb();
        len = rx_sw_len[tail];
        if (len > 0U && len <= ETH_BUF_SIZE)
            udp_stack_rx_handle(rx_sw_fbuf[tail], len);
        dmb();
        rx_sw_tail = (tail + 1U) & RX_SW_QUEUE_MASK;
    }
}

static void eth_err_isr(void *CallBackRef, u8 Direction, u32 ErrorCode)
{
    XEmacPs *p = (XEmacPs *)CallBackRef;
    /* 驱动 XEmacPs_IntrHandler 在分发前已清 ISR（此处读 ISR 恒 0，无意义）；
     * 真实错误码由第 3 参传 TXSR/RXSR 值，用它定位。
     * Direction: SEND=1, RECV=2。RX err bit0=BUFFNA, bit2=RXOVR, bit3=HRESP。 */
    if (Direction == XEMACPS_RECV) {
        if (ErrorCode & XEMACPS_RXSR_BUFFNA_MASK)
            eth_rx_buffna_cnt++;
        if (ErrorCode & XEMACPS_RXSR_RXOVR_MASK)
            eth_rx_overrun_cnt++;
    }
    eth_mac_err_cnt++;
    eth_mac_err_last = ((u32)Direction << 24) | (ErrorCode & 0x00FFFFFFU);
    /* emacps 驱动已在 BUFFNA 时 flush RX SRAM。若该次中断没有
     * 同时带 FRAMERX，RecvHandler 可能未被调用；主动扫一次
     * 已完成 BD 并重挂，避免 RXUSED 状态持续。 */
    if (Direction == XEMACPS_RECV &&
        (ErrorCode & (XEMACPS_RXSR_BUFFNA_MASK | XEMACPS_RXSR_RXOVR_MASK))) {
        u32 nwctrl;
        eth_rx_recovery_cnt++;
        eth_rx_isr(p);
        /* 重挂 BD 后显式保持 RXEN，并清理 DPRAM 内那个因无缓冲
         * 未能投递的残留包，使偶发 BUFFNA 能自动恢复。 */
        nwctrl = XEmacPs_ReadReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET);
        XEmacPs_WriteReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET,
                nwctrl | XEMACPS_NWCTRL_RXEN_MASK | XEMACPS_NWCTRL_FLUSH_DPRAM_MASK);
    }
}

/* ==================== TX 缓冲分配（轮转） ==================== */
u8 *eth_tx_get_buf(void)
{
    u32 irq_flags = eth_irq_save();
    u8 *p = tx_fbuf[tx_fbuf_idx % TX_FRAME_BUFS];
    tx_fbuf_idx++;
    eth_irq_restore(irq_flags);
    return p;
}

/* ==================== TX 发送（非阻塞） ==================== */
int eth_tx_send(const u8 *buf, u16 len)
{
    XEmacPs_Bd *Bd;
    LONG st;
    u32 irq_flags;
    u32 bd_index;
    u8 *dma_buf;

    if (!eth_link_up_flag)
        return -1;
    if (buf == NULL || len == 0U || len > ETH_BUF_SIZE)
        return -2;

    /* BdRingAlloc/ToHw 与 TX 完成 ISR 的 FromHw/Free 必须串行。
     * pending++ 也须与 ToHw 处在同一临界区：GEM 正在运行时，
     * 新 BD 在 ToHw 后可能立刻完成并中断。旧顺序先 ToHw、
     * 再 pending++，长时运行可偶发丢失一次回收计数，最终
     * pending 虚假累积到32，上层误判 UDP TX 环永久满。 */
    irq_flags = eth_irq_save();
    st = XEmacPs_BdRingAlloc(&Emac.TxBdRing, 1, &Bd);
    if (st != XST_SUCCESS) {
        eth_tx_alloc_fail_cnt++;
        eth_irq_restore(irq_flags);
        return -3;   /* 无可用 BD，丢包 */
    }

    /* IRQ exclusion protects BD bookkeeping; it does not stop GEM DMA.
     * Publish payload first, then the descriptor giving hardware ownership. */
    bd_index = (u32)(Bd - tx_bd_ring);
    dma_buf = tx_dma_payload[bd_index];
    memcpy(dma_buf, buf, len);
    Xil_DCacheFlushRange((UINTPTR)dma_buf, len);
    dsb();
    XEmacPs_BdSetAddressTx(Bd, (UINTPTR)dma_buf);
    XEmacPs_BdSetLength(Bd, len);
    XEmacPs_BdSetLast(Bd);
    XEmacPs_BdClearTxNoCRC(Bd);          /* 让 MAC 补 FCS */
    if ((UINTPTR)Bd == (UINTPTR)&tx_bd_ring[TX_BD_CNT - 1])
        eth_bd_tx_wrap(Bd);              /* 环尾 TX BD 设 wrap（DMA 回绕环首） */
    dsb();
    XEmacPs_BdClearTxUsed(Bd); /* ownership publication MUST be the last store */
    Xil_DCacheFlushRange((UINTPTR)Bd, sizeof(XEmacPs_Bd));
    dsb();
    XEmacPs_BdRingToHw(&Emac.TxBdRing, 1, Bd);
    tx_ring_buffers_in_hw++;
    XEmacPs_Transmit(&Emac);             /* 写 STARTTX */
    eth_irq_restore(irq_flags);
    return 0;
}

void eth_tx_reclaim(void) { /* ISR 已做，占位兼容 */ }
u32 eth_tx_pending(void) { return tx_ring_buffers_in_hw; }
u32 eth_tx_completed(void) { return eth_tx_done_cnt; }
u32 eth_tx_alloc_failures(void) { return eth_tx_alloc_fail_cnt; }
u32 eth_tx_counter_faults(void) { return eth_tx_counter_fault_cnt; }
u32 eth_rx_buffer_errors(void) { return eth_rx_buffna_cnt; }
u32 eth_rx_overrun_errors(void) { return eth_rx_overrun_cnt; }
u32 eth_rx_hw_owned(void) { return Emac.RxBdRing.HwCnt; }
u32 eth_rx_sw_drops(void) { return eth_rx_sw_drop_cnt; }
u32 eth_rx_queue_depth(void)
{
    return (rx_sw_head - rx_sw_tail) & RX_SW_QUEUE_MASK;
}
u32 eth_rx_recoveries(void) { return eth_rx_recovery_cnt; }
u32 eth_mac_error_count(void) { return eth_mac_err_cnt; }
u32 eth_mac_last_error(void) { return eth_mac_err_last; }
u32 eth_rx_irq_faults(void) { return eth_rx_irq_fault_cnt; }

/* ==================== 建链 ==================== */
/* Both boot and explicit communication reset use the same ring setup. */
static int eth_create_rings(void)
{
    XEmacPs_Bd *BdSet;
    u32 i;
    Xil_SetTlbAttributes((INTPTR)&bd_memory, NORM_NONCACHE);
    /* ---- BD ring 建立（Start 之前） ---- */
    if (XEmacPs_BdRingCreate(&Emac.TxBdRing, (UINTPTR)tx_bd_ring,
                             (UINTPTR)tx_bd_ring, XEMACPS_BD_ALIGNMENT, TX_BD_CNT)
            != XST_SUCCESS) {
        xil_printf("[eth] TX ring create fail\r\n");
        return -4;
    }
    /* ★ TX 环关键修复：空闲 BD 必须预置 USED=1（软件拥有，DMA 跳过）。
     * emacps 的 BdRingCreate 把整环 memset 成 0 → word1=0 → USED(bit31)=0 →
     * GEM TX DMA 把全部空闲 BD 都当作"已就绪可发" → boot ARP 之后自动连读
     * 31 个全零 BD 当垃圾帧（长度 0、无 LAST）→ 环尾无 wrap 直接走出环外 →
     * DMA 状态机与驱动环指针错乱 → 之后 hw-done 不再触发、TX 永久失效、
     * `MAC err` 刷屏、PC 从未收到 Zynq 任何有效帧（与本板实测逐条吻合）。
     * DMA 发完一个 BD 会自己置回 USED=1；软件只需把"空闲"BD 标为 USED=1。
     * RX 环有显式预载（全 16 BD 置地址+清 NEW 再 ToHw），TX 环此前没有——此修复
     * 就是补上 TX 侧的对等预载。 */
    for (i = 0; i < TX_BD_CNT; i++)
        XEmacPs_BdSetTxUsed(&tx_bd_ring[i]);
    eth_bd_tx_wrap(&tx_bd_ring[TX_BD_CNT - 1]);   /* 环尾 wrap（与 RX 同源） */
    if (XEmacPs_BdRingCreate(&Emac.RxBdRing, (UINTPTR)rx_bd_ring,
                             (UINTPTR)rx_bd_ring, XEMACPS_BD_ALIGNMENT, RX_BD_CNT)
            != XST_SUCCESS) {
        xil_printf("[eth] RX ring create fail\r\n");
        return -5;
    }
    Xil_DCacheFlushRange((UINTPTR)tx_bd_ring, sizeof(tx_bd_ring));
    Xil_DCacheFlushRange((UINTPTR)rx_bd_ring, sizeof(rx_bd_ring));

    /* 预载 RX BD（立即能收 ARP 应答） */
    if (XEmacPs_BdRingAlloc(&Emac.RxBdRing, RX_BD_CNT, &BdSet) != XST_SUCCESS) {
        xil_printf("[eth] RX ring preload alloc fail\r\n");
        return -6;
    }
    for (i = 0; i < RX_BD_CNT; i++) {
        XEmacPs_BdSetAddressRx(&BdSet[i], (UINTPTR)&rx_fbuf[i][0]);
        XEmacPs_BdClearRxNew(&BdSet[i]);
    }
    eth_bd_rx_wrap(&BdSet[RX_BD_CNT - 1]);   /* 环尾 BD 设 wrap，DMA 才能回绕 */
    Xil_DCacheFlushRange((UINTPTR)BdSet, RX_BD_CNT * sizeof(XEmacPs_Bd));
    if (XEmacPs_BdRingToHw(&Emac.RxBdRing, RX_BD_CNT, BdSet) != XST_SUCCESS) {
        xil_printf("[eth] RX ring preload to-hw fail\r\n");
        return -7;
    }

    /* RX 缓冲 cache 预清（Xilinx 文档推荐模式：缓冲分配后、DMA 启动前 invalidate）。
     * fbuf 是 BSS(全零)，启动前可能仍脏在 L2；先整块对齐失效（首尾均 32B 对齐，
     * 无破坏性 partial flush），保证 DMA 写入 DDR 后 ISR 读回的是帧数据而非缓存旧零。 */
    Xil_DCacheInvalidateRange((UINTPTR)rx_fbuf, sizeof(rx_fbuf));
    return 0;
}

int eth_network_reset(void)
{
    u32 flags;
    int st;
    if (!eth_initialized)
        return -1;
    flags = eth_irq_save();
    XEmacPs_Stop(&Emac);
    dsb();
    usleep(50); /* bounded drain of any final GEM DMA transaction */
    st = eth_create_rings();
    if (st == 0) {
        tx_ring_buffers_in_hw = 0U;
        tx_fbuf_idx = 0U;
        rx_sw_head = rx_sw_tail = 0U;
        XEmacPs_WriteReg(Emac.Config.BaseAddress, XEMACPS_RXSR_OFFSET, 0xFU);
        XEmacPs_WriteReg(Emac.Config.BaseAddress, XEMACPS_TXSR_OFFSET, 0xFFU);
        XEmacPs_Start(&Emac);
    }
    eth_irq_restore(flags);
    xil_printf("[eth] NET RESET status=%d rx_hw=%u tx_pending=%u; RF/BIT unchanged\r\n",
               st, (unsigned)Emac.RxBdRing.HwCnt, (unsigned)tx_ring_buffers_in_hw);
    return st;
}

int eth_link_init(void)
{
    XEmacPs_Config *cfg;
    int st;

    if (eth_initialized)
        return 0;   /* 幂等 */

    rx_sw_head = 0U;
    rx_sw_tail = 0U;
    eth_rx_sw_drop_cnt = 0U;
    eth_rx_recovery_cnt = 0U;
    eth_link_up_flag = eth_phy_configured = 0U;
    eth_link_poll_at = 0U;
    eth_link_phase = 0U;

    cfg = XEmacPs_LookupConfig(XPAR_XEMACPS_0_DEVICE_ID);
    if (!cfg) {
        xil_printf("[eth] GEM0 config not found\r\n");
        return -1;
    }
    if (XEmacPs_CfgInitialize(&Emac, cfg, cfg->BaseAddress) != XST_SUCCESS) {
        xil_printf("[eth] GEM0 CfgInitialize fail\r\n");
        return -2;
    }
    XEmacPs_SetMacAddress(&Emac, eth_local_mac, 1);   /* Index 1-4; 0 触发断言死循环 */

    /* 选项：MAC 补 FCS；关收发校验和卸载（SW 手写校验和，避免 MAC 覆盖） */
    XEmacPs_SetOptions(&Emac, XEMACPS_FCS_INSERT_OPTION);
    XEmacPs_ClearOptions(&Emac,
            XEMACPS_RX_CHKSUM_ENABLE_OPTION | XEMACPS_TX_CHKSUM_ENABLE_OPTION);

    /* UG585：MDC由CPU_1x分频，不是RGMII TX时钟。默认/32仅适合<=80MHz。
     * 当前P351改用/224，覆盖CPU_1x<=540MHz的合法范围，保守低于2.5MHz。 */
    XEmacPs_SetMdioDivisor(&Emac, MDC_DIV_224);

    /* PHY 扫描 + 建链 */
    st = eth_phy_scan();
    if (st == (int)ETH_PHY_NONE) {
        xil_printf("[eth] no PHY found (MDIO hang?)\r\n");
        return -3;
    }
    eth_phy_setup((u8)st);   /* 内含RTL8211F延迟配置和AN；无网线不影响本地控制。 */
    /* RGMII 时延强制：Zynq GEM 不提供 2ns 偏置，必须 PHY 开 TX+RX 时延。
     * 设置后铜线链路不受影响（时延只管 GEM↔PHY 的 RGMII 接口）。 */
    /* 延迟已在PHY复位后、AN前配置。不能在这里写旧RTL8211E扩展页。 */

    st = eth_create_rings();
    if (st != 0)
        return st;

    /* 中断挂接（复用 dma_rx 的 GIC 实例） */
    if (dma_rx_gic_instance() != NULL) {
        XScuGic_Connect(dma_rx_gic_instance(), ETH_GEM_INTR_ID,
                (Xil_ExceptionHandler)XEmacPs_IntrHandler, &Emac);
        XScuGic_Enable(dma_rx_gic_instance(), ETH_GEM_INTR_ID);
    } else {
        xil_printf("[eth] WARN: dma_rx GIC not ready, no GEM IRQ\r\n");
    }

    XEmacPs_SetHandler(&Emac, XEMACPS_HANDLER_DMASEND, eth_tx_isr, &Emac);
    XEmacPs_SetHandler(&Emac, XEMACPS_HANDLER_DMARECV, eth_rx_isr, &Emac);
    XEmacPs_SetHandler(&Emac, XEMACPS_HANDLER_ERROR,   eth_err_isr, &Emac);

    XEmacPs_Start(&Emac);
    eth_initialized = 1;
    {
        /* AN 协商明细：BMSR bit5=AN 完成；ANLPAR=链路伙伴广告（bit8=100FD bit7=100HD） */
        u16 bmsr = 0U, anlpar = 0U;
        eth_phy_reg_read(eth_phy_addr, PHY_REG_BMSR, &bmsr);
        eth_phy_reg_read(eth_phy_addr, PHY_REG_BMSR, &bmsr);
        eth_phy_reg_read(eth_phy_addr, PHY_REG_ANLPAR, &anlpar);
        xil_printf("[eth] GEM0 up, TX ring=%u RX ring=%u link=%u nwctrl=0x%08x anCpl=%u anLP=0x%04x\r\n",
                (unsigned)TX_BD_CNT, (unsigned)RX_BD_CNT, (unsigned)eth_link_up_flag,
                (unsigned)XEmacPs_ReadReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET),
                (unsigned)((bmsr >> 5) & 1u), (unsigned)anlpar);
    }
    return 0;
}

int eth_link_deinit(void)
{
    if (!eth_initialized)
        return 0;
    XEmacPs_Stop(&Emac);
    if (dma_rx_gic_instance() != NULL)
        XScuGic_Disable(dma_rx_gic_instance(), ETH_GEM_INTR_ID);
    eth_initialized = 0;
    eth_link_up_flag = eth_phy_configured = 0U;
    eth_link_phase = 0U;
    return 0;
}
