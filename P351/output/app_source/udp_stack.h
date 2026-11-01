/* ============================================================================
 * udp_stack.h —— 手写 ARP/IPv4/UDP 打包 + ARP 应答解析（ZSDR 协议回传用）
 *
 * 冻结协议（与 rx_udp_ref/z7sdr_py_ref.py 逐字节一致）：
 *   UDP 数据口 40001（PS→PC 单向）。
 *   控制口 40000（PC→PS）：载荷 4 字节 'SNAP' = 立即采集一次（串口不可用时的
 *   PC UI「立即采集」触发；数据帧本身即确认，无需 ACK）。
 *   帧 = 以太头(14) + IPv4(20) + UDP(8) + ZSDR 头(32) + int16[4]×n_groups。
 *   ZSDR 32B 头（小端）：magic='ZSDR' ver=1 flags[bit0=END,bit1=err] u16 pkt_len
 *     u32 capture_id u16 frame_index u16 frame_count u16 n_groups u16 rsvd
 *     u32 ts_lo u32 ts_hi u32 rsvd。
 *
 * Board IP: 192.168.10.2. Controller/data destination learned dynamically.
 * 校验和：IPv4头必须正确；接收非零UDP校验和按RFC768验证，零值保留IPv4兼容。
 * 发出的UDP校验和仍为0。控制命令按完整报文定长匹配，不接受多余尾字节。
 * 本模块同时收 ARP 应答 → 维护 peer_mac cache。
 * ==========================================================================*/
#ifndef UDP_STACK_H
#define UDP_STACK_H

#include "xil_types.h"
#include "board_config.h"
#include "screen_v2.h"

/* PC整机请求只排入前台队列，IRQ/UDP解析中不直接调谐或开启RF。 */
u8 udp_ctrl_final_rf_get(screen_command_t *command);
void udp_ctrl_final_rf_complete(u32 sequence,u32 op,u32 result);

/* Foreground-only AD9361 sensor reading in milli-Celsius; INT32_MIN=invalid. */
s32 board_temperature_mC(void);
/* 非零：整机最终RF事务正在换频或有效发射，屏幕拥有RF/码源控制权。
 * 由main实现；UDP只读查询及真正停发仍可使用。不能用弱默认0绕过所有权。 */
int board_final_rf_owned(void);
/* 所有权期间被拒绝的控制请求使用原ACK magic+nonce并返回非零BUSY=3；
 * TFS结果保留其错误位定义(3<<16)。无专用ACK的旧命令返回BUSY magic。
 * TXM00以及CRC有效的TXB1 STOP仍可中止整机；STATUS/查询不改变RF任务。 */
u32 board_rx_lo_khz(void);
u32 board_rx_bw_hz(void);

/* Diagnostic only: AD9361-internal TX tone bypasses the FPGA LVDS data port.
 * action 0=off, 1=30 s tone, 2=query; result bit16=active, low16=error. */
u32 board_tx_bist_diag(u32 action);

/* Full AD9361 TX RF-bandwidth range accepted by TXCF.  The active hardware
 * is ID_AD9361; the separate AD9363 20 MHz ceiling does not apply. */
#define AD9361_TX_RF_BW_MIN_HZ 200000UL
#define AD9361_TX_RF_BW_MAX_HZ 56000000UL

/* ---- 网络常量 ---- */
#define ETH_IP_ADDR0     192
#define ETH_IP_ADDR1     168
#define ETH_IP_ADDR2     10
#define ETH_IP_ADDR3     2        /* 本机 Zynq */

#define UDP_PORT_DATA    40001
#define UDP_PORT_CTRL    40000
#define ZSDR_HDR_LEN     32

#define ETHTYPE_IPV4     0x0800
#define ETHTYPE_ARP      0x0806
#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2

/* ---- ZSDR 32B 头（按冻结协议，小端打包） ---- */
typedef struct {
    u8  magic[4];        /* 'Z','S','D','R' */
    u8  ver;             /* 1 */
    u8  flags;           /* bit0=END, bit1=err */
    u16 pkt_len;         /* 32+载荷 总长 */
    u32 capture_id;
    u16 frame_index;
    u16 frame_count;
    u16 n_groups;        /* 8..160 */
    u16 rsvd0;
    u32 ts_lo;
    u32 ts_hi;
    u32 rsvd1;
} zsdr_hdr_t;

/* ---- API ---- */
void udp_stack_init(void);                       /* 清零 ARP cache 等 */
void udp_network_service(void);                  /* Main-only NRES service; preserves TX. */
u16  ip_checksum(const u8 *data, u32 len);       /* RFC1071 */
int  udp_build_arp_request(u8 *buf, u16 *len);   /* 以太+ARP 请求，广播 */
int  udp_build_zsdr_frame(u8 *buf, u16 *len,     /* 组装完整 UDP 帧 */
                          const zsdr_hdr_t *h,
                          const s16 *payload, u16 payload_bytes);
void udp_stack_rx_handle(const u8 *buf, u16 len); /* 收帧：ARP 应答→peer_mac、ARP 请求→应答、ICMP→echo */
void udp_stack_rx_stats(u32 *total, u32 *arp, u32 *ipv4);  /* 诊断：RX 帧统计 */
void udp_stack_rx_reset(void);                             /* 诊断：清统计+重打前帧 */
u32  udp_stack_rx_arp_us(void);              /* 诊断：收到目标 IP=本机的 ARP 请求数（PC 在找我们） */
u32  udp_stack_rx_icmp_us(void);             /* 诊断：收到目标 IP=本机的 ICMP echo 请求数（PC 真在 ping 我们） */
u32  udp_stack_arp_reply_tx(void);           /* 诊断：我们成功提交的 ARP 应答数 */
u32  udp_stack_arp_req_tx(void);             /* 诊断：我们发出的 who-has 广播数（外部 TX 活则持续涨） */
u8   udp_peer_mac(u8 mac[6]);                    /* 1=已解析, 0=未知 */
u8   udp_arp_resolve(void);                      /* 发一次 ARP 请求；0=已发 */
u32  udp_capture_id_next(void);                  /* 递增 capture_id */
u8   udp_peer_ok(void);

/* ---- 控制口（UDP 40000）立即采集 'SNAP' 命令 ----
 * ISR只入队，eth_rx_poll在主循环每次解析一帧；
 * app_upload 主循环 peek→就绪后 clear→采一次。一次一采，不依赖使能/DWELL。 */
void udp_ctrl_capture_request(void);   /* 主循环解析'SNAP'后置位 */
void udp_ctrl_monitor_request(void);   /* SNM1：4096组；不改变RF、MONS或码源 */
u32  udp_ctrl_capture_groups(void);    /* 请求消费前读取，SNAP/STR1仍为131072组 */
u8   udp_ctrl_capture_pending(void);   /* 1=有待采请求（只读不消费） */
void udp_ctrl_capture_clear(void);     /* 消费请求（就绪后调，下一次生效） */

/* Dual-TX UI control. Packet formats on UDP/40000:
 *   "TXM" + tx1_mode + tx2_mode, mode: 0=OFF, 1=QPSK, 2=FM,
 *                                      3=SOQPSK-TG, 4=PCM/FM,
 *                                      5=retired/silent, 6=complex sine/CW
 *   "TXCF" + mode1:u8 + mode2:u8 + reserved:u16 + lo_hz:u64le +
 *            bandwidth_hz:u32le (AD9361 TX filter 0.2..56 MHz) +
 *            tx1_atten_mdB:u32le + tx2_atten_mdB:u32le
 *   UI r27 presents calibrated connector power and converts it to these
 *   relative attenuation fields.  mode=0 is a real PS RF gate, not mute gain.
 *            reserved bits: [2:0] line_code (NRZ-L/M/S, Bi-phase-L/M/S, RNRZ-L),
 *            [6:4] rolloff preset
 *   "REFC" + version:u8 + source:u8 + reserved:u16 + ref_clk_hz:u32le
 *      v2 appends nonce:u32le; after application/verification the board sends
 *      "RFAK" + nonce:u32le + result:u32le (0=verified, 1=invalid,
 *      2=old PL, 3=quiesce failed, 4=clock setup/lock failed).
 *   "PCMF" + version=2:u8 + enable:u8 + filter_profile:u8 + randomizer:u8 +
 *            bit_rate_word:u32le + deviation_word:u32le +
 *            frequency_offset_word:u32le + amplitude:u32le
 *   "SQPK" + version:u8 + enable:u8 + differential:u8 + randomizer:u8 +
 *            bit_rate_word:u32le + frequency_offset_word:u32le + amplitude:u32le
 *   "LBCK" + version=1:u8 + mode:u8 + reserved:u16le
 *            mode 0=normal RX, 1=AD9361 internal TX->RX digital loopback
 *   "MONS" + version=1:u8 + source:u8 + reserved:u16le
 *            source 0=RX ADC, 1=TX DAC (default/recommended)
 *   "RFRX" + version=1:u8 + enable:u8 + reserved:u16le
 *            enable external RF receive for SMA self-test: RX LO follows TX LO,
 *            RX bandwidth follows TX bandwidth, RX gain is reduced for loopback.
 *   "PING" + nonce:u32le -> "PONG" + same nonce + status:u32le(0).
 *   "NRES" + nonce:u32le -> "NRAK" + same nonce + status:u32le(0=success).
 *      NRES rebuilds GEM rings and stops monitoring; RF/BIT state is preserved.
 *      Replies go to the request source port, NOT fixed data port 40001.
 *   "TXST" requests a combined BIT/DMA, PL waveform and AD9361 RF-gate
 *            snapshot on the Vitis serial console.
 *   "STR0" / "STR1" stop/start continuous RX upload. */
u8   udp_ctrl_tx_mode_get(u8 *tx1_mode, u8 *tx2_mode);
/* 只查看尚未消费的TXMD/TXCF停发，不删除命令。 */
u8   udp_ctrl_tx_stop_pending(void);
u8   udp_ctrl_tx_config_get(u8 *tx1_mode, u8 *tx2_mode, u64 *lo_hz,
                            u32 *bandwidth_hz, u32 *tx1_atten_mdB,
                            u32 *tx2_atten_mdB, u8 *line_code,
                            u8 *rolloff);
u8   udp_ctrl_refclk_config_get(u8 *source, u32 *rate_hz);
void udp_ctrl_refclk_complete(u32 result);
void udp_ctrl_refclk_invalidate(void);
u8   udp_ctrl_pcm_config_get(u32 *bit_rate_word, u32 *deviation_word,
                             u32 *frequency_offset_word,
                             u16 *amplitude, u8 *filter_profile, u8 *randomizer,
                             u8 *enable);
u8   udp_ctrl_soqpsk_config_get(u32 *bit_rate_word, u32 *frequency_offset_word,
                                u16 *amplitude, u8 *differential, u8 *randomizer,
                                u8 *enable);
u8   udp_ctrl_loopback_get(u8 *mode);
u8   udp_ctrl_monitor_source_get(u8 *source);
u8   udp_ctrl_rf_rx_get(u8 *enable);
u8   udp_ctrl_stream_get(u8 *on);
u8   udp_ctrl_tx_status_get(void);

#endif /* UDP_STACK_H */
