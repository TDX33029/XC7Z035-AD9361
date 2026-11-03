/* ============================================================================
 * udp_stack.c —— ARP/IPv4/UDP 打包 + ARP 应答解析
 *
 * 帧布局（全部网络字节序 big-endian，ARM 手动字节序写入）：
 *   [0..5]  dst MAC
 *   [6..11] src MAC
 *   [12:13] EtherType 0x0800 (IPv4)
 *   [14..33] IPv4 头 (20B, IHL=5, no options)
 *   [34..41] UDP 头 (8B)
 *   [42..73] ZSDR 32B 头（小端）
 *   [74..]  int16 LE 载荷
 *
 * ARP 请求：
 *   [0..5] FF:FF:FF:FF:FF:FF
 *   [6..11] src MAC
 *   [12:13] 0x0806
 *   [14..41] ARP 头 (28B)
 * ==========================================================================*/
#include "udp_stack.h"
#include "udp_dhcp.h"
#include "eth_link.h"
#include "tx_bit_protocol.h"
#include "app_upload.h"
#include "tfs_control.h"
#include "parameters.h"
#include "rx_receiver.h"
#include "final_rf_status.h"
#include <string.h>
#include "xtime_l.h"
#include "xpseudo_asm.h"
#include "xil_printf.h"

#define UDP_CPSR_I_BIT 0x80U

/* ---- 本机 MAC（与 eth_link 保持一致） ---- */
static const u8 local_mac[6] = { ETH_LOCAL_MAC0, ETH_LOCAL_MAC1, ETH_LOCAL_MAC2,
                                 ETH_LOCAL_MAC3, ETH_LOCAL_MAC4, ETH_LOCAL_MAC5 };

static u8  peer_mac[6]      = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static u8  peer_mac_valid   = 0;
static u8 board_ip[4]={ETH_IP_ADDR0,ETH_IP_ADDR1,ETH_IP_ADDR2,ETH_IP_ADDR3};
static u8 board_mask[4]={255,255,0,0}, board_gateway[4];
static u8 rollback_ip[4], rollback_mask[4], rollback_gateway[4], config_owner[4];
static u32 config_nonce;
static XTime config_deadline;
static u8 config_trial;
static u8 peer_ip[4]; /* Learned from PING or a capture/stream-start request. */
static u32 eth_capture_id   = 0;
static u8 net_reset_frame[82]; /* Ethernet + maximum IPv4 header + UDP */
static u32 net_reset_ihl, net_reset_nonce, net_reset_result;
static u8 net_reset_pending, net_reset_seen;
static u8 tfs_last_request[24], tfs_last_valid;
static u8 tfs_last_ip[4], tfs_last_mac[6];
static u16 tfs_last_port;
static u8 refclk_reply_frame[82], refclk_last_request[16];
static u32 refclk_reply_ihl, refclk_reply_result;
static u8 refclk_reply_pending, refclk_reply_done;
static u8 refclk_inflight;
static u8 receiver_last_request[48],receiver_last_valid,receiver_last_length;
static u8 receiver_last_ip[4],receiver_last_mac[6];
static u16 receiver_last_port;
static u8 receiver_peer_ip[4],receiver_peer_mac[6],receiver_peer_valid;
static screen_command_t final_command;
static u8 final_command_pending,final_command_valid;
static u8 final_command_wire[63],final_command_ip[4],final_command_mac[6];
static u16 final_command_port;
static u32 final_command_nonce,final_command_result;

u8 udp_ctrl_final_rf_get(screen_command_t *command)
{
    if(!command || !final_command_pending) return 0U;
    *command=final_command;final_command_pending=0U;return 1U;
}
void udp_ctrl_final_rf_complete(u32 sequence,u32 op,u32 result)
{
    if(final_command_valid && sequence==final_command.sequence && op==final_command.op)
        final_command_result=result;
}

/* 控制包由eth_rx_poll在主循环每次解析一帧，再由主循环消费。
 * ISR仅搬运完整以太帧；下面的配置不能放回中断内执行。 */
static volatile u8 udp_ctrl_capture_req = 0;
static u32 udp_ctrl_capture_size = UDP_CAPTURE_GROUPS;
static volatile u8 udp_ctrl_tx_pending = 0;
static volatile u8 udp_ctrl_tx1_mode = 0;
static volatile u8 udp_ctrl_tx2_mode = 0;
static volatile u8 udp_ctrl_tx_config_pending = 0;
static volatile u8 udp_ctrl_cfg_tx1_mode = 0;
static volatile u8 udp_ctrl_cfg_tx2_mode = 0;
static volatile u64 udp_ctrl_cfg_lo_hz = 2400000000ULL;
static volatile u32 udp_ctrl_cfg_bandwidth_hz = 20000000UL;
static volatile u32 udp_ctrl_cfg_tx1_atten_mdB = 10000UL;
static volatile u32 udp_ctrl_cfg_tx2_atten_mdB = 10000UL;
static volatile u8 udp_ctrl_cfg_line_code = 0;
static volatile u8 udp_ctrl_cfg_rolloff = 0;
static volatile u8 udp_ctrl_refclk_config_pending = 0;
static volatile u8 udp_ctrl_refclk_source = 0;
static volatile u32 udp_ctrl_refclk_rate_hz = 40000000UL;
static volatile u8 udp_ctrl_pcm_config_pending = 0;
static volatile u32 udp_ctrl_pcm_bit_rate_word = 0;
static volatile u32 udp_ctrl_pcm_deviation_word = 0;
static volatile u32 udp_ctrl_pcm_frequency_offset_word = 0;
static volatile u16 udp_ctrl_pcm_amplitude = 0;
static volatile u8 udp_ctrl_pcm_filter_profile = 0;
static volatile u8 udp_ctrl_pcm_randomizer = 0;
static volatile u8 udp_ctrl_pcm_enable = 0;
static volatile u8 udp_ctrl_soqpsk_config_pending = 0;
static volatile u32 udp_ctrl_soqpsk_bit_rate_word = 0;
static volatile u32 udp_ctrl_soqpsk_frequency_offset_word = 0;
static volatile u16 udp_ctrl_soqpsk_amplitude = 0;
static volatile u8 udp_ctrl_soqpsk_differential = 1;
static volatile u8 udp_ctrl_soqpsk_randomizer = 0;
static volatile u8 udp_ctrl_soqpsk_enable = 0;
static volatile u8 udp_ctrl_loopback_pending = 0;
static volatile u8 udp_ctrl_loopback_mode = 0;
static volatile u8 udp_ctrl_monitor_source_pending = 0;
static volatile u8 udp_ctrl_monitor_source = 1;
static volatile u8 udp_ctrl_rf_rx_pending = 0;
static volatile u8 udp_ctrl_rf_rx_enable = 0;
static volatile u8 udp_ctrl_stream_pending = 0;
static volatile u8 udp_ctrl_stream_on = 0;
static volatile u8 udp_ctrl_tx_status_pending = 0;

/* 保留短临界区以保护配置快照；不要把volatile等同于多字段事务原子性。 */
static u32 udp_irq_save(void)
{
    u32 cpsr = mfcpsr();
    mtcpsr(cpsr | UDP_CPSR_I_BIT);
    return cpsr;
}

static void udp_irq_restore(u32 cpsr)
{
    mtcpsr(cpsr);
}

void udp_stack_init(void)
{
    peer_mac_valid = 0;
    eth_capture_id = 0;
    udp_ctrl_capture_req = 0U;
    udp_ctrl_capture_size = UDP_CAPTURE_GROUPS;
    udp_ctrl_tx_status_pending = 0;
    net_reset_pending = net_reset_seen = 0U;
    tfs_last_valid = 0U;
    receiver_last_valid=receiver_peer_valid=0;
    final_command_pending=final_command_valid=0U;
    final_command_nonce=final_command_result=0U;
    memset(&final_command,0,sizeof final_command);
}

/* DHCP 是唯一允许在运行期改写本机地址的来源。地址变化后旧的 ARP
 * 对端、数据流接收方和 NCFG trial 状态都不再可信，必须全部作废。 */
void udp_stack_set_addresses(const u8 *ip, const u8 *mask, const u8 *gw)
{
    if (!ip || !mask || !gw) return;
    /* 续租通常返回同一地址；无变化时不得中断正在进行的采集/数据流。 */
    if (!memcmp(board_ip, ip, 4U) && !memcmp(board_mask, mask, 4U) &&
        !memcmp(board_gateway, gw, 4U)) return;
    memcpy(board_ip, ip, 4U);
    memcpy(board_mask, mask, 4U);
    memcpy(board_gateway, gw, 4U);
    peer_mac_valid = 0U;
    memset(peer_ip, 0, 4U);
    config_trial = 0U;
    config_nonce = 0U;
    udp_stream_force(0U);
    udp_ctrl_capture_clear();
    xil_printf("[net] local address %u.%u.%u.%u/%u.%u.%u.%u\r\n",
        board_ip[0], board_ip[1], board_ip[2], board_ip[3],
        board_mask[0], board_mask[1], board_mask[2], board_mask[3]);
}

const u8 *udp_stack_local_ip(void)
{
    return board_ip;
}

/* RFC1071 校验和（网络序）；奇数长度末字节按低8位补零处理。 */
u16 ip_checksum(const u8 *data, u32 len)
{
    u32 sum = 0;
    u32 i;
    for (i = 0; i + 1 < len; i += 2)
        sum += ((u32)data[i] << 8) | data[i+1];
    if (len & 1)
        sum += (u32)data[len-1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

/* 写入 16 位网络字节序（大端） */
static void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
/* 写入 16 位小端 */
static void put_le16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put_le32(u8 *p, u32 v) { p[0]=(u8)v;p[1]=(u8)(v>>8);p[2]=(u8)(v>>16);p[3]=(u8)(v>>24); }

/* 读 16 位大端 */
static u16 get_be16(const u8 *p) { return (u16)((p[0]<<8) | p[1]); }
static u32 get_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 get_le64(const u8 *p)
{
    return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24) |
           ((u64)p[4] << 32) | ((u64)p[5] << 40) | ((u64)p[6] << 48) | ((u64)p[7] << 56);
}
static u16 get_le16(const u8 *p){return (u16)p[0] | ((u16)p[1]<<8);}

static u8 mac_is_unicast(const u8 *mac)
{
    return !(mac[0] & 1U) &&
           (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0U;
}

/* RFC768：非零UDP校验和必须覆盖伪首部、UDP头和完整载荷。
 * IPv4的零校验和表示发送端未提供校验，保留旧控制端兼容性。
 * 调用前已核验IP/UDP长度；不把以太填充字节计入校验。 */
static u8 udp_checksum_valid(const u8 *frame, u32 offset, u32 length)
{
    u32 sum;
    if (get_be16(frame + offset + 6U) == 0U) return 1U;
    sum = (u16)~ip_checksum(frame + 26U, 8U);
    sum += 17U + length;
    sum += (u16)~ip_checksum(frame + offset, length);
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return sum == 0xFFFFU;
}

/* Reply directly to the request MAC/IP/source port, independent of ARP cache
 * and of the fixed IQ data port 40001. Payload: magic + nonce + status (LE). */
static void udp_control_reply(const u8 *request, u32 ihl,
                              const char *magic, u32 nonce, u32 status)
{
    u8 *out = eth_tx_get_buf();
    u16 checksum;
    u32 extra = memcmp(magic, "TFA2", 4U) == 0 ? 16U : 0U;
    memset(out, 0, 60U + extra);
    memcpy(out, request + 6, 6U);
    memcpy(out + 6, local_mac, 6U);
    put_be16(out + 12, ETHTYPE_IPV4);
    out[14] = 0x45; out[22] = 64; out[23] = 17;
    put_be16(out + 16, (u16)(40U + extra));
    memcpy(out + 26, board_ip, 4U);
    memcpy(out + 30, request + 26, 4U);
    checksum = ip_checksum(out + 14, 20U);
    put_be16(out + 24, checksum);
    put_be16(out + 34, UDP_PORT_CTRL);
    put_be16(out + 36, get_be16(request + 14U + ihl));
    put_be16(out + 38, (u16)(20U + extra));
    memcpy(out + 42, magic, 4U);
    put_le32(out + 46, nonce);
    put_le32(out + 50, status);
    if (extra) {
        u32 words[4], i;
        tfs_control_snapshot(words);
        for (i=0U;i<4U;i++) put_le32(out+54U+4U*i,words[i]);
    }
    eth_tx_send(out, (u16)(extra ? 70U : 60U));
}

/* 整机屏幕事务拥有中频、参考、本振及后级衰减时，旧网络控制不能插入。
 * 拦截位于所有去重缓存之前：重复的旧START/TFS写也不能重放成功ACK。
 * 白名单只包含真正只读请求和STOP；TFS 0x81/82/86虽然读取器件，仍会
 * 改PL opcode并占用SPI事务，不属于op0/5这种纯寄存器快照查询。 */
static u8 udp_final_rf_guard(const u8 *request, u32 ihl,
                            const u8 *p, u32 length)
{
    u32 nonce = length >= 8U ? get_le32(p + 4U) : 0U;
    const char *ack = "BUSY";
    u32 result = 3U;
    if (!board_final_rf_owned()) return 0U;
    if ((length==71U && memcmp(p,"FRCM",4U)==0) ||
        (length == 8U && (memcmp(p,"FRCQ",4U)==0 || memcmp(p,"PING",4U)==0 ||
         memcmp(p,"CFGQ",4U)==0 || memcmp(p,"FRFQ",4U)==0 || memcmp(p,"RDFQ",4U)==0 ||
         memcmp(p,"TMPQ",4U)==0 ||
         memcmp(p,"RXQ1",4U)==0 || memcmp(p,"RXSQ",4U)==0 || memcmp(p,"IQSZ",4U)==0 ||
         memcmp(p,"RXFQ",4U)==0 || memcmp(p,"RXBW",4U)==0)) ||
        (length == 12U && memcmp(p,"BQRY",4U)==0) ||
        (length == 12U && memcmp(p,"TBDG",4U)==0 && get_le32(p+8U)==2U) ||
        (length == 4U && memcmp(p,"TXST",4U)==0) ||
        (length == 5U && memcmp(p,"TXM",3U)==0 && !p[3] && !p[4]))
        return 0U;
    if (length >= 4U && memcmp(p,"TXB1",4U)==0) {
        /* 完整STOP/STATUS继续交给原始协议核验CRC、版本、通道等。
         * CLEAR不是整机STOP：它还清数据/所有权，因此不在白名单。 */
        if (length == TXB1_HEADER_SIZE && p[4] == 1U &&
            (p[5] == 5U || p[5] == 7U) && get_le16(p+24U)==0U)
            return 0U;
        ack = "TBAK";
        nonce = length >= TXB1_HEADER_SIZE ? get_le32(p+36U) : 0U;
    } else if (length >= 4U && (memcmp(p,"TFS1",4U)==0 || memcmp(p,"TFS2",4U)==0)) {
        if (length == 24U && p[8] == 1U && (p[9] == 0U || p[9] == 5U) &&
            get_le32(p+20U)==0U) return 0U;
        ack = p[3]=='2' ? "TFA2" : "TFAK";
        result = 3U << 16;
    } else if (length >= 4U && memcmp(p,"REFC",4U)==0) {
        ack="RFAK";nonce=length>=16U?get_le32(p+12U):0U;
    } else if (length >= 4U && memcmp(p,"TBDG",4U)==0) {
        ack="TBDA";
    } else if (length >= 4U && (memcmp(p,"RXC1",4U)==0 || memcmp(p,"RXC2",4U)==0)) {
        ack="RXAK";
    } else if (length >= 4U && memcmp(p,"NCIP",4U)==0) {
        ack="NCAK";
    } else if (length >= 4U && memcmp(p,"NCCF",4U)==0) {
        ack="NCCK";
    } else if (length >= 4U && memcmp(p,"NRES",4U)==0) {
        ack="NRAK";
    }
    udp_control_reply(request,ihl,ack,nonce,result);
    return 1U;
}

/* Called after eth_rx_poll has released its software queue slot. */
/* CFGQ只查询，不学习采集目的地址、不改RF、不重置网络；每包返回一致参数快照。 */
static void udp_config_reply(const u8 *request, u32 ihl, u32 nonce)
{
    u32 words[BOARD_CONFIG_WORDS], i;
    u32 payload = 8U + 4U * BOARD_CONFIG_WORDS;
    u8 *out = eth_tx_get_buf();
    board_config_snapshot(words);
    /* 同一批收包中尚待执行的TXCF/参考切换，不能让旧快照冒充新配置已完成。 */
    if (udp_ctrl_tx_config_pending || udp_ctrl_refclk_config_pending)
        words[BC_FLAGS] &= ~BOARD_CONFIG_RF_VALID;
    memset(out, 0, 42U + payload);
    memcpy(out, request + 6, 6U);
    memcpy(out + 6, local_mac, 6U);
    put_be16(out + 12, ETHTYPE_IPV4);
    out[14] = 0x45; out[22] = 64; out[23] = 17;
    put_be16(out + 16, (u16)(28U + payload));
    memcpy(out + 26, board_ip, 4U);
    memcpy(out + 30, request + 26, 4U);
    put_be16(out + 24, ip_checksum(out + 14, 20U));
    put_be16(out + 34, UDP_PORT_CTRL);
    put_be16(out + 36, get_be16(request + 14U + ihl));
    put_be16(out + 38, (u16)(8U + payload));
    memcpy(out + 42, "CFGA", 4U);
    put_le32(out + 46, nonce);
    for (i = 0U; i < BOARD_CONFIG_WORDS; i++)
        put_le32(out + 50U + 4U * i, words[i]);
    eth_tx_send(out, (u16)(42U + payload));
}

/* 独立接收快照返回软件配置与PL数字计数，不是射频功率/灵敏度测量。 */
static void udp_final_rf_reply(const u8 *request,u32 ihl,u32 nonce)
{
    u32 words[FINAL_RF_STATUS_WORDS],i;
    u32 payload=8U+4U*FINAL_RF_STATUS_WORDS;
    u8 *out=eth_tx_get_buf();
    board_final_rf_snapshot(words);
    memset(out,0,42U+payload);
    memcpy(out,request+6,6U); memcpy(out+6,local_mac,6U);
    put_be16(out+12,ETHTYPE_IPV4);
    out[14]=0x45U; out[22]=64U; out[23]=17U;
    put_be16(out+16,(u16)(28U+payload));
    memcpy(out+26,board_ip,4U); memcpy(out+30,request+26,4U);
    put_be16(out+24,ip_checksum(out+14,20U));
    put_be16(out+34,UDP_PORT_CTRL); put_be16(out+36,get_be16(request+14U+ihl));
    put_be16(out+38,(u16)(8U+payload));
    memcpy(out+42,"FRFA",4U); put_le32(out+46,nonce);
    for(i=0U;i<FINAL_RF_STATUS_WORDS;i++) put_le32(out+50U+4U*i,words[i]);
    eth_tx_send(out,(u16)(42U+payload));
}

/* RDFQ+nonce -> RDFA+nonce+20字：最近一次整机请求的原始值（含被拒请求）。
 * 只读，不占用RF所有权、不访问SPI；result字段区分已应用与未就绪/忙/失败。 */
static void udp_request_draft_reply(const u8 *request,u32 ihl,u32 nonce)
{
    u32 words[FINAL_RF_REQUEST_WORDS],i;
    u32 payload=8U+4U*FINAL_RF_REQUEST_WORDS;
    u8 *out=eth_tx_get_buf();
    board_final_rf_request_snapshot(words);
    memset(out,0,42U+payload);
    memcpy(out,request+6,6U); memcpy(out+6,local_mac,6U);
    put_be16(out+12,ETHTYPE_IPV4);
    out[14]=0x45U; out[22]=64U; out[23]=17U;
    put_be16(out+16,(u16)(28U+payload));
    memcpy(out+26,board_ip,4U); memcpy(out+30,request+26,4U);
    put_be16(out+24,ip_checksum(out+14,20U));
    put_be16(out+34,UDP_PORT_CTRL); put_be16(out+36,get_be16(request+14U+ihl));
    put_be16(out+38,(u16)(8U+payload));
    memcpy(out+42,"RDFA",4U); put_le32(out+46,nonce);
    for(i=0U;i<FINAL_RF_REQUEST_WORDS;i++) put_le32(out+50U+4U*i,words[i]);
    eth_tx_send(out,(u16)(42U+payload));
}

/* FRCM=magic+nonce+原生63字节v6请求；同端点/nonce重发只回缓存，不重启任务。
 * 本函数仅排队。FRCI能力字0x00060E01明确区分旧固件，不猜测支持整机控制。 */
static void udp_final_rf_command(const u8 *request,u32 ihl,const u8 *p)
{
    screen_command_t c;
    u32 nonce=get_le32(p+4U),result=SC_PARAM;
    u16 port=get_be16(request+14U+ihl);
    if(final_command_valid && nonce==final_command_nonce && port==final_command_port &&
       !memcmp(final_command_ip,request+26,4U) && !memcmp(final_command_mac,request+6,6U)) {
        result=memcmp(final_command_wire,p+8U,63U)?SC_SEQUENCE:final_command_result;
    } else if(screen_command_decode(p+8U,63U,&c) && c.op==SC_UP70_PRECISE_TX) {
        if(board_final_rf_owned() || final_command_pending) result=SC_BUSY;
        else {
            final_command=c;final_command_pending=final_command_valid=1U;
            final_command_nonce=nonce;final_command_port=port;
            memcpy(final_command_wire,p+8U,63U);memcpy(final_command_ip,request+26,4U);
            memcpy(final_command_mac,request+6,6U);
            final_command_result=result=SC_PENDING;
        }
    }
    udp_control_reply(request,ihl,"FRCA",nonce,result);
}

/* 独立接收快照返回软件配置与PL数字计数，不是射频功率/灵敏度测量。 */
static void udp_receiver_reply(const u8 *request,u32 ihl,u32 nonce)
{
    u32 w[RXR_CFG_WORDS],i;u8 *out=eth_tx_get_buf();
    rx_receiver_snapshot(w);
    memset(out,0,50U+4U*RXR_CFG_WORDS);
    memcpy(out,request+6,6);memcpy(out+6,local_mac,6);put_be16(out+12,ETHTYPE_IPV4);
    out[14]=0x45;out[22]=64;out[23]=17;put_be16(out+16,36U+4U*RXR_CFG_WORDS);
    memcpy(out+26,board_ip,4);memcpy(out+30,request+26,4);put_be16(out+24,ip_checksum(out+14,20));
    put_be16(out+34,UDP_PORT_CTRL);put_be16(out+36,get_be16(request+14+ihl));
    put_be16(out+38,16U+4U*RXR_CFG_WORDS);memcpy(out+42,"RXA1",4);put_le32(out+46,nonce);
    for(i=0;i<RXR_CFG_WORDS;i++)put_le32(out+50+4*i,w[i]);
    (void)eth_tx_send(out,50U+4U*RXR_CFG_WORDS);
}

int udp_send_receiver_bits(const u32 *words,u16 count,u32 epoch,u32 sequence,
                           u32 bit_offset,u32 rate,u32 status)
{
    zsdr_hdr_t h;u16 length,k;s16 payload[256];u8 *out;
    if(count==0 || count>128 || !eth_link_is_up() || !receiver_peer_valid || eth_tx_pending()>=TX_BD_CNT-2U)return -1;
    memset(&h,0,sizeof h);memcpy(h.magic,"RXB1",4);h.ver=1;h.flags=(u8)(status&15U);
    h.pkt_len=32U+4U*count;h.capture_id=epoch;h.frame_index=(u16)sequence;h.frame_count=(u16)(sequence>>16);
    h.n_groups=count;h.ts_lo=bit_offset;h.ts_hi=rate;h.rsvd1=status;
    for(k=0;k<count;k++){payload[2*k]=(s16)words[k];payload[2*k+1]=(s16)(words[k]>>16);}
    out=eth_tx_get_buf();udp_build_zsdr_frame(out,&length,&h,payload,4U*count);
    /* 独立端口40002；旧IQ/ZSDR40001不变，旧解析器不能误认位流为I/Q。 */
    put_be16(out+34,40002U);put_be16(out+36,40002U);
    memcpy(out,receiver_peer_mac,6);memcpy(out+30,receiver_peer_ip,4);
    put_be16(out+24,0);put_be16(out+24,ip_checksum(out+14,20));
    return eth_tx_send(out,length);
}

/* Called after eth_rx_poll has released its software queue slot. */
void udp_network_service(void)
{
    int st;
    XTime now;
    XTime_GetTime(&now);
    if (config_trial && now >= config_deadline) {
        memcpy(board_ip,rollback_ip,4U);
        memcpy(board_mask,rollback_mask,4U);
        memcpy(board_gateway,rollback_gateway,4U);
        config_trial=0U; peer_mac_valid=0U;
        xil_printf("[netcfg] confirmation timeout; IP restored %u.%u.%u.%u\r\n",
            board_ip[0],board_ip[1],board_ip[2],board_ip[3]);
    }
    if (!net_reset_pending)
        return;
    net_reset_pending = 0U;
    udp_stream_force(0U);
    udp_ctrl_capture_clear();
    st = eth_network_reset();
    net_reset_result = (st == 0) ? 0U : 1U;
    net_reset_seen = 1U;
    udp_control_reply(net_reset_frame, net_reset_ihl, "NRAK",
                      net_reset_nonce, net_reset_result);
}

static const u8 *peer_next_hop(void)
{
    unsigned j;
    for(j=0;j<4U;++j) {
        if ((peer_ip[j]&board_mask[j])!=(board_ip[j]&board_mask[j]) &&
            (board_gateway[0]|board_gateway[1]|board_gateway[2]|board_gateway[3]))
            return board_gateway;
    }
    return peer_ip;
}

/* ---- ARP 请求（广播，问 peer/网关 IP 的 MAC） ---- */
int udp_build_arp_request(u8 *buf, u16 *len)
{
    u32 o = 0;
    int i;
    if (!peer_mac_valid) return -1;
    /* 以太头 */
    for (i = 0; i < 6; i++) buf[o++] = 0xFF;
    for (i = 0; i < 6; i++) buf[o++] = local_mac[i];
    put_be16(&buf[o], ETHTYPE_ARP); o += 2;
    /* ARP 头 (28B) */
    put_be16(&buf[o], 1); o += 2;           /* htype Ethernet */
    put_be16(&buf[o], 0x0800); o += 2;      /* ptype IPv4 */
    buf[o++] = 6;                            /* hlen */
    buf[o++] = 4;                            /* plen */
    put_be16(&buf[o], ARP_OP_REQUEST); o += 2;
    for (i = 0; i < 6; i++) buf[o++] = local_mac[i];   /* sender MAC */
    buf[o++] = board_ip[0]; buf[o++] = board_ip[1];
    buf[o++] = board_ip[2]; buf[o++] = board_ip[3];  /* sender IP */
    for (i = 0; i < 6; i++) buf[o++] = 0x00;           /* target MAC=0 */
    memcpy(buf + o, peer_next_hop(), 4U); o += 4;
    *len = (u16)o;
    return 0;
}

/* ---- ZSDR UDP 帧组装 ---- */
int udp_build_zsdr_frame(u8 *buf, u16 *len,
                         const zsdr_hdr_t *h,
                         const s16 *payload, u16 payload_bytes)
{
    u32 o = 0;
    u16 ip_tot, udp_len;
    u16 i;
    u8  iph[20];

    /* ---- 以太头 ---- */
    for (i = 0; i < 6; i++) buf[o++] = peer_mac[i];
    for (i = 0; i < 6; i++) buf[o++] = local_mac[i];
    put_be16(&buf[o], ETHTYPE_IPV4); o += 2;

    /* ---- IPv4 头 (20B) ---- */
    ip_tot  = (u16)(20 + 8 + 32 + payload_bytes);
    udp_len = (u16)(8 + 32 + payload_bytes);
    iph[0] = 0x45;                    /* ver=4, IHL=5 */
    iph[1] = 0x00;                    /* ToS */
    put_be16(&iph[2], ip_tot);
    put_be16(&iph[4], (u16)(h->capture_id & 0xFFFF));  /* id */
    put_be16(&iph[6], 0x0000);        /* frag */
    iph[8]  = 64;                     /* TTL */
    iph[9]  = 17;                     /* proto UDP */
    put_be16(&iph[10], 0);            /* checksum 占位 */
    iph[12] = board_ip[0]; iph[13] = board_ip[1];
    iph[14] = board_ip[2]; iph[15] = board_ip[3];
    memcpy(iph + 16, peer_ip, 4U);
    put_be16(&iph[10], ip_checksum(iph, 20));
    for (i = 0; i < 20; i++) buf[o++] = iph[i];

    /* ---- UDP 头 (8B) ---- */
    put_be16(&buf[o], UDP_PORT_DATA); o += 2;   /* sport */
    put_be16(&buf[o], UDP_PORT_DATA); o += 2;   /* dport */
    put_be16(&buf[o], udp_len);      o += 2;
    put_be16(&buf[o], 0x0000);       o += 2;    /* checksum=0 */

    /* ---- ZSDR 32B 头（小端） ---- */
    buf[o++] = h->magic[0]; buf[o++] = h->magic[1];
    buf[o++] = h->magic[2]; buf[o++] = h->magic[3];
    buf[o++] = h->ver;
    buf[o++] = h->flags;
    put_le16(&buf[o], h->pkt_len);  o += 2;
    put_le32(&buf[o], h->capture_id); o += 4;
    put_le16(&buf[o], h->frame_index); o += 2;
    put_le16(&buf[o], h->frame_count); o += 2;
    put_le16(&buf[o], h->n_groups); o += 2;
    put_le16(&buf[o], h->rsvd0);    o += 2;
    put_le32(&buf[o], h->ts_lo);    o += 4;
    put_le32(&buf[o], h->ts_hi);    o += 4;
    put_le32(&buf[o], h->rsvd1);    o += 4;

    /* ---- 载荷 int16 LE [i0 q0 i1 q1]×n_groups ---- */
    for (i = 0; i < payload_bytes / 2; i++) {
        put_le16(&buf[o], (u16)payload[i]);   /* payload 是 s16，低16位 */
        o += 2;
    }
    *len = (u16)o;
    return 0;
}

/* ---- 收帧统计 ---- */
static u32 rx_total_frames = 0, rx_arp_cnt = 0, rx_ipv4_cnt = 0, rx_other_cnt = 0;
static u32 rx_arp_us = 0;        /* ARP who-has 目标 IP=本机（PC 真的在找我们） */
static u32 rx_icmp_us = 0;       /* ICMP echo 请求目标 IP=本机（PC 真的在 ping 我们） */
static u32 arp_reply_tx_cnt = 0; /* 我们成功提交的 ARP 应答数（PC 却收不到 → TX 外部死） */
static u32 arp_req_tx_cnt = 0;   /* 我们发出的 who-has 广播数（外部 TX 活则持续涨） */

/* ---- ARP 应答：有人问本机 IP，回 is-at（让 PC 也能解析我们） ---- */
static void udp_arp_respond(const u8 *in, u16 len)
{
    u8 *out;
    int i;
    (void)len;
    out = eth_tx_get_buf();   /* 稳定缓冲：BD 指向它，GEM DMA 异步读，不可用栈 */
    /* 以太头：dst=原 sender MAC, src=本机 */
    for (i = 0; i < 6; i++) { out[i] = in[6 + i]; out[6 + i] = local_mac[i]; }
    put_be16(&out[12], ETHTYPE_ARP);
    /* ARP 头（len>=42 已保证） */
    out[14]=in[14]; out[15]=in[15];                 /* htype */
    out[16]=in[16]; out[17]=in[17];                 /* ptype */
    out[18]=in[18]; out[19]=in[19];                 /* hlen plen */
    put_be16(&out[20], ARP_OP_REPLY);
    for (i = 0; i < 6; i++) out[22 + i] = local_mac[i];    /* sender MAC=本机 */
    out[28]=board_ip[0]; out[29]=board_ip[1];
    out[30]=board_ip[2]; out[31]=board_ip[3];            /* sender IP=本机 */
    for (i = 0; i < 6; i++) out[32 + i] = in[22 + i];      /* target MAC=原 sender */
    out[38]=in[28]; out[39]=in[29]; out[40]=in[30]; out[41]=in[31];  /* target IP */
    if (eth_tx_send(out, 42) == 0)   /* 0=入 TX 环成功；非 0=静默丢包（-3=BD 满） */
        arp_reply_tx_cnt++;
}

/* ---- ICMP echo 应答（ping 通，端到端验证） ---- */
static void udp_icmp_respond(const u8 *in, u16 len)
{
    u8 *out;
    u32 o = 14;
    u8 ihl;
    u32 po;
    u16 sum, cs;
    int i;
    if (len > ETH_BUF_SIZE) return;
    ihl = (u8)((in[o] & 0x0F) * 4);
    po = o + ihl;                     /* ICMP 起点 */
    if (po + 8 > len) return;
    if (in[po] != 8U || in[po+1U] != 0U ||
        ip_checksum(in + po, len - po) != 0U) return;
    /* 只回复发给本机的合法echo，不对IP广播ping应答。 */
    if (in[o+16] != board_ip[0] || in[o+17] != board_ip[1] ||
        in[o+18] != board_ip[2] || in[o+19] != board_ip[3])
        return;
    rx_icmp_us++;   /* PC 真的在 ping 本机 */
    out = eth_tx_get_buf();
    for (i = 0; i < (int)len; i++) out[i] = in[i];     /* 整体复制到稳定缓冲 */
    /* 回复源MAC始终是本机，不能把输入帧的广播目标当成回复源。 */
    memcpy(out, in + 6U, 6U);
    memcpy(out + 6U, local_mac, 6U);
    /* 交换 IP src/dst（IP 头内 12..15 ↔ 16..19） */
    for (i = 0; i < 4; i++) { u8 t = out[o+12+i]; out[o+12+i] = out[o+16+i]; out[o+16+i] = t; }
    /* ICMP: type=0 echo reply, code=0, checksum 重算（头+数据） */
    out[po] = 0; out[po+1] = 0; out[po+2] = 0; out[po+3] = 0;
    sum = ip_checksum(&out[po], len - po);
    out[po+2] = (u8)(sum >> 8); out[po+3] = (u8)sum;
    /* IP 校验和重算（src/dst 变了） */
    out[o+10] = 0; out[o+11] = 0;
    cs = ip_checksum(&out[o], ihl);
    out[o+10] = (u8)(cs >> 8); out[o+11] = (u8)cs;
    eth_tx_send(out, len);
}

/* ---- 收帧：ARP 应答学习 peer_mac + ARP 请求应答 + ICMP echo ---- */
void udp_stack_rx_handle(const u8 *buf, u16 len)
{
    u16 etype;
    u32 o;
    int i;

    if (!buf || len < 14U || len > ETH_BUF_SIZE) return;
    if ((memcmp(buf, local_mac, 6U) != 0 &&
         memcmp(buf, "\xff\xff\xff\xff\xff\xff", 6U) != 0) ||
        !mac_is_unicast(buf + 6U)) return;
    etype = get_be16(&buf[12]);
    rx_total_frames++;
    if (etype == ETHTYPE_ARP) rx_arp_cnt++;
    else if (etype == ETHTYPE_IPV4) rx_ipv4_cnt++;
    else rx_other_cnt++;

    if (etype == ETHTYPE_ARP && len >= 42) {
        u16 op = get_be16(&buf[20]);
        /* 这里只支持Ethernet/IPv4 ARP；先核验长度类型和发送端一致性，
         * 再应答或学习，避免把其他格式按固定偏移误解析为控制端。 */
        if (get_be16(buf + 14U) != 1U ||
            get_be16(buf + 16U) != ETHTYPE_IPV4 ||
            buf[18] != 6U || buf[19] != 4U ||
            memcmp(buf + 22U, buf + 6U, 6U) != 0) return;
        if (op == ARP_OP_REQUEST) {
            /* who-has 本机 IP? → 应答；target IP 必须是我们 */
            if (buf[38] == board_ip[0] && buf[39] == board_ip[1] &&
                buf[40] == board_ip[2] && buf[41] == board_ip[3]) {
                rx_arp_us++;
                udp_arp_respond(buf, len);
            }
            return;
        }
        if (op != ARP_OP_REPLY)
            return;
        /* 应答是给本机的（target IP 须是本机） */
        if (buf[38] != board_ip[0] || buf[39] != board_ip[1] ||
            buf[40] != board_ip[2] || buf[41] != board_ip[3] ||
            memcmp(buf + 32U, local_mac, 6U) != 0)
            return;
        /* sender 必须是已配对的 PC */
        if (!peer_mac_valid || memcmp(buf + 28, peer_next_hop(), 4U) != 0)
            return;
        /* Refreshing an unchanged ARP entry is routine, not a UART event. */
        i = (!peer_mac_valid || memcmp(peer_mac, buf + 22, 6U) != 0) ? 1U : 0U;
        memcpy(peer_mac, buf + 22, 6U);
        peer_mac_valid = 1;
        if (i && !rx_receiver_active())
            xil_printf("[udp] ARP peer=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
                    peer_mac[0], peer_mac[1], peer_mac[2],
                    peer_mac[3], peer_mac[4], peer_mac[5]);
    } else if (etype == ETHTYPE_IPV4 && len >= 42) {
        u32 ihl = (u32)(buf[14] & 15U) * 4U;
        u32 ip_len = get_be16(buf + 16);
        u32 udp_len;
        if ((buf[14] >> 4) != 4U || ihl < 20U || ihl > 60U ||
            ip_len < ihl || 14U + ip_len > len ||
            (get_be16(buf + 20) & 0xBFFFU) || buf[22] == 0U ||
            ip_checksum(buf + 14, ihl) != 0U)
            return; /* no fragment reassembly; never parse padding as payload */
        /* DHCP 必须先于目的地址过滤：OFFER/ACK 可能单播到尚未配置的
         * yiaddr，续租 ACK 单播到 ciaddr。只接受服务器(67)→客户端(68)。 */
        if (buf[23] == 17U) {
            u32 uoff = 14U + ihl;
            u16 ulen = 0U;
            if (uoff + 8U <= len) ulen = get_be16(buf + uoff + 4U);
            if (uoff + 8U <= len && get_be16(buf + uoff) == 67U &&
                get_be16(buf + uoff + 2U) == 68U && ulen >= 8U &&
                uoff + ulen <= len && udp_checksum_valid(buf, uoff, ulen)) {
                udp_dhcp_rx(buf + uoff + 8U, ulen - 8U);
                return;
            }
        }
        if (memcmp(buf+30,board_ip,4U)!=0 &&
            memcmp(buf+30,"\xff\xff\xff\xff",4U)!=0)
            return;
        len = (u16)(14U + ip_len);
        o = 14;
        /* 有效单播源地址不固定为某台PC；此检查也适用于ICMP。 */
        if (buf[o+12] == 0U || buf[o+12] == 127U || buf[o+12] >= 224U)
            return;
        if (buf[o+9] == 1) {           /* ICMP → echo 应答（ping） */
            udp_icmp_respond(buf, len);
            return;
        }
        if (buf[o+9] != 17) return;    /* 非 UDP，忽略 */
        o += ihl;
        if (o + 8U > len) return;
        udp_len = get_be16(buf + o + 4U);
        if (udp_len < 12U || o + udp_len > len) return;
        if (get_be16(&buf[o+2]) != UDP_PORT_CTRL) return;  /* dport */
        if (!udp_checksum_valid(buf, o, udp_len)) return;
        len = (u16)(o + udp_len);
        o += 8;    /* UDP 头 */
        if (o + 4 > len) return;
        /* Broadcast permits discovery ONLY, never RF or address changes. */
        if (len-o==8U && memcmp(buf+o,"DSCI",4U)==0) {
            udp_control_reply(buf,ihl,"DSCA",get_le32(buf+o+4U),0U);
            return;
        }
        if (memcmp(buf+30,board_ip,4U)!=0) return;
        if (udp_final_rf_guard(buf,ihl,buf+o,(u32)(len-o))) return;
        if (len-o==20U && memcmp(buf+o,"NCIP",4U)==0) {
            const u8 *ip=buf+o+8U, *mask=ip+4U, *gw=mask+4U;
            u32 ipn=((u32)ip[0]<<24)|((u32)ip[1]<<16)|((u32)ip[2]<<8)|ip[3];
            u32 mn=((u32)mask[0]<<24)|((u32)mask[1]<<16)|((u32)mask[2]<<8)|mask[3];
            u32 gn=((u32)gw[0]<<24)|((u32)gw[1]<<16)|((u32)gw[2]<<8)|gw[3];
            u32 inv=~mn, nonce=get_le32(buf+o+4U);
            u32 result=0U;
            if (!ip[0] || ip[0]==127U || ip[0]>=224U || !mn || inv<3U ||
                (inv&(inv+1U)) || !(ipn&inv) || (ipn&inv)==inv ||
                memcmp(ip,buf+26,4U)==0 ||
                (gn && ((gn&mn)!=(ipn&mn) || !(gn&inv) || (gn&inv)==inv || gn==ipn)))
                result=1U;
            if (config_trial) result=2U;
            udp_control_reply(buf,ihl,"NCAK",nonce,result);
            if (!result) {
                memcpy(rollback_ip,board_ip,4U); memcpy(rollback_mask,board_mask,4U);
                memcpy(rollback_gateway,board_gateway,4U);
                memcpy(config_owner,buf+26,4U); config_nonce=nonce;
                memcpy(board_ip,ip,4U); memcpy(board_mask,mask,4U); memcpy(board_gateway,gw,4U);
                XTime_GetTime(&config_deadline);
                config_deadline+=(XTime)COUNTS_PER_SECOND*30U;
                config_trial=1U; peer_mac_valid=0U;
                /* 手工 NCIP 优先于 DHCP：暂停租约，避免续租把地址改回去。 */
                udp_dhcp_suspend();
                udp_stream_force(0U); udp_ctrl_capture_clear();
                xil_printf("[netcfg] trial IP=%u.%u.%u.%u; confirm within 30s\r\n",
                    board_ip[0],board_ip[1],board_ip[2],board_ip[3]);
            }
            return;
        }
        if (len-o==8U && memcmp(buf+o,"NCCF",4U)==0) {
            u32 nonce=get_le32(buf+o+4U);
            u32 ok=(nonce==config_nonce && memcmp(config_owner,buf+26,4U)==0);
            if (ok) config_trial=0U;
            udp_control_reply(buf,ihl,"NCCK",nonce,ok?0U:1U);
            return;
        }
        /* Only an explicit probe or capture start changes the data recipient.
         * Reply packets independently target their actual request sender.
         * Use the received Ethernet source as next hop, including routers. */
        if ((len-o == 8U && memcmp(buf+o,"PING",4U)==0) ||
            (len-o == 4U && (memcmp(buf+o,"SNAP",4U)==0 || memcmp(buf+o,"SNM1",4U)==0 ||
                             memcmp(buf+o,"STR1",4U)==0))) {
            memcpy(peer_ip,buf+26,4U);
            memcpy(peer_mac,buf+6,6U);
            peer_mac_valid=1U;
        }
        /* Foreground-only, bounded register access. Does not block on LD.
         * TFS1: nonce32, version8=1, op8, half_period16, value64, reserved32.
         * Retransmission of the last accepted write returns status, without
         * restarting the synthesizer's frequency acquisition. */
        if (len - o == 24U && (memcmp(buf + o, "TFS1", 4U) == 0 ||
                              memcmp(buf + o, "TFS2", 4U) == 0)) {
            const u8 *p = buf + o;
            u32 result;
            if (p[8] != 1U || (p[9] > 5U && p[9] != 0x81U &&
                p[9] != 0x82U && p[9] != 0x86U) || get_le32(p + 20U) != 0U) {
                result = 2U << 16;
            } else if (p[9] != 0U && tfs_last_valid &&
                       memcmp(tfs_last_request, p, 24U) == 0 &&
                       memcmp(tfs_last_ip, buf + 26U, 4U) == 0 &&
                       memcmp(tfs_last_mac, buf + 6U, 6U) == 0 &&
                       tfs_last_port == get_be16(buf + 14U + ihl)) {
                result = tfs_control_status();
            } else {
                u64 value = (u64)get_le32(p + 12U) |
                            ((u64)get_le32(p + 16U) << 32);
                result = tfs_control_request(p[9],
                    (u16)((u16)p[10] | ((u16)p[11] << 8)), value);
                if (p[9] != 0U && !(result & 0x00FF0000U)) {
                    memcpy(tfs_last_request, p, 24U);
                    memcpy(tfs_last_ip, buf + 26U, 4U);
                    memcpy(tfs_last_mac, buf + 6U, 6U);
                    tfs_last_port = get_be16(buf + 14U + ihl);
                    tfs_last_valid = 1U;
                }
            }
            udp_control_reply(buf, ihl, p[3]=='2' ? "TFA2" : "TFAK",
                              get_le32(p + 4U), result);
            return;
        }
        if (len - o == 8U && memcmp(buf + o, "PING", 4U) == 0) {
            udp_control_reply(buf, ihl, "PONG", get_le32(buf + o + 4U), 0U);
            return;
        }
        /* 只读能力查询；旧ELF不回复，新UI可明确提示使用长帧兼容模式。 */
        if (len-o==8U && memcmp(buf+o,"IQSZ",4U)==0) {
            udp_control_reply(buf,ihl,"IQSA",get_le32(buf+o+4U),UDP_MONITOR_GROUPS);
            return;
        }
        if ((len-o==40U && memcmp(buf+o,"RXC1",4)==0) || (len-o==48U && memcmp(buf+o,"RXC2",4)==0)) {
            const u8 *p=buf+o;rx_receiver_config_t cfg;u32 result=1U,request_length=len-o;
            if(receiver_last_valid && receiver_last_length==request_length && memcmp(receiver_last_request,p,request_length)==0 &&
                memcmp(receiver_last_ip,buf+26,4)==0 && memcmp(receiver_last_mac,buf+6,6)==0 &&
                receiver_last_port==get_be16(buf+14U+ihl)){
                udp_control_reply(buf,ihl,"RXAK",get_le32(p+4),0);return;
            }
            /* 新事务即使失败，也可能已停止旧任务；旧成功ACK不能继续命中。 */
            receiver_last_valid=0;
            if(p[9]<=1U && ((request_length==40U && p[8]==1U && get_le16(p+34)==1U) ||
                (request_length==48U && p[8]==2U && get_le16(p+46)==0U))){
                memset(&cfg,0,sizeof cfg);cfg.enabled=p[9];cfg.mode=p[10];cfg.line_code=p[11];
                cfg.center_hz=get_le64(p+12);cfg.bit_rate=get_le32(p+20);cfg.bandwidth_hz=get_le32(p+24);
                cfg.gain_db=(s32)get_le32(p+28);cfg.frame_bytes=get_le16(p+32);cfg.frame_count=get_le16(p+34);
                cfg.sync_word=request_length==48U?get_le64(p+36):get_le32(p+36);
                cfg.sync_bits=request_length==48U?p[44]:32U;cfg.sfid_start=request_length==48U?p[45]:0U;
                result=rx_receiver_apply(&cfg);
                if(result==0){
                    memcpy(receiver_last_request,p,request_length);receiver_last_length=(u8)request_length;receiver_last_valid=1;
                    memcpy(receiver_last_ip,buf+26,4);memcpy(receiver_last_mac,buf+6,6);
                    receiver_last_port=get_be16(buf+14U+ihl);
                    if(cfg.enabled){memcpy(receiver_peer_ip,buf+26,4);memcpy(receiver_peer_mac,buf+6,6);receiver_peer_valid=1;}
                }
            }
            udp_control_reply(buf,ihl,"RXAK",get_le32(p+4),result);return;
        }
        if (len-o==8U && memcmp(buf+o,"RXQ1",4)==0){udp_receiver_reply(buf,ihl,get_le32(buf+o+4));return;}
        if (len-o==8U && memcmp(buf+o,"FRFQ",4)==0){udp_final_rf_reply(buf,ihl,get_le32(buf+o+4));return;}
        if (len-o==8U && memcmp(buf+o,"RDFQ",4)==0){udp_request_draft_reply(buf,ihl,get_le32(buf+o+4));return;}
        if (len-o==8U && memcmp(buf+o,"FRCQ",4)==0){udp_control_reply(buf,ihl,"FRCI",get_le32(buf+o+4),0x00060E01U);return;}
        if (len-o==71U && memcmp(buf+o,"FRCM",4)==0){udp_final_rf_command(buf,ihl,buf+o);return;}
        if (len - o == 8U && memcmp(buf + o, "CFGQ", 4U) == 0) {
            udp_config_reply(buf, ihl, get_le32(buf + o + 4U));
            return;
        }
        if (len - o == 8U && memcmp(buf + o, "RXSQ", 4U) == 0) {
            udp_control_reply(buf, ihl, "RXSA", get_le32(buf + o + 4U),
                              board_rx_status());
            return;
        }
        if (len - o == 8U && memcmp(buf + o, "RXFQ", 4U) == 0) {
            udp_control_reply(buf, ihl, "RXFA", get_le32(buf + o + 4U),
                              board_rx_lo_khz());
            return;
        }
        if (len - o == 8U && memcmp(buf + o, "RXBW", 4U) == 0) {
            udp_control_reply(buf, ihl, "RXBA", get_le32(buf + o + 4U),
                              board_rx_bw_hz());
            return;
        }
        if (len - o == 8U && memcmp(buf + o, "TMPQ", 4U) == 0) {
            /* Does not change the IQ stream destination or RF configuration. */
            udp_control_reply(buf, ihl, "TMPA", get_le32(buf + o + 4U),
                              (u32)board_temperature_mC());
            return;
        }
        if (len - o == 12U && memcmp(buf + o, "TBDG", 4U) == 0) {
            u32 action = get_le32(buf + o + 8U);
            /* This does not alter the PL waveform. The internal AD9361 tone
             * is explicitly requested and automatically expires after 30 s. */
            udp_control_reply(buf, ihl, "TBDA", get_le32(buf + o + 4U),
                              board_tx_bist_diag(action));
            return;
        }
        if (len - o == 8U && memcmp(buf + o, "NRES", 4U) == 0) {
            u32 nonce = get_le32(buf + o + 4U);
            if (net_reset_seen && nonce == net_reset_nonce &&
                ihl == net_reset_ihl &&
                memcmp(buf + 26, net_reset_frame + 26, 4U) == 0 &&
                get_be16(buf + 14U + ihl) ==
                get_be16(net_reset_frame + 14U + net_reset_ihl)) {
                udp_control_reply(buf, ihl, "NRAK", nonce, net_reset_result);
            } else {
                memcpy(net_reset_frame, buf, 14U + ihl + 8U);
                net_reset_ihl = ihl;
                net_reset_nonce = nonce;
                net_reset_pending = 1U;
            }
            return;
        }
        /* Binary arbitrary-BIT download/control protocol. It consumes the
         * datagram when the TXB1 magic is present, including rejected TXB1
         * packets, so malformed binary data cannot fall through as ASCII. */
        if (len - o == 12U && memcmp(buf + o, "BQRY", 4U) == 0) {
            udp_control_reply(buf, ihl, "BQAK", get_le32(buf + o + 4U),
                              tx_bit_protocol_query(get_le32(buf + o + 8U)));
            return;
        }
        if (tx_bit_protocol_handle(&buf[o], (u16)(len - o))) {
            if (len - o >= TXB1_HEADER_SIZE) {
                u32 result = tx_bit_protocol_ack_result();
                /* 仅原协议实际接受(CRC/长度/版本合法)的STOP映射为整机STOP。
                 * 不能只停BIT DMA：待执行的整机换频也必须取消，后级转安全衰减。
                 * 复用TXM00邮箱，使主循环udp_ctrl_tx_stop_pending优先消费。 */
                if (board_final_rf_owned() && len-o==TXB1_HEADER_SIZE &&
                    buf[o+4U]==1U && buf[o+5U]==5U && result==0U) {
                    udp_ctrl_tx1_mode=0U;
                    udp_ctrl_tx2_mode=0U;
                    udp_ctrl_tx_pending=1U;
                }
                udp_control_reply(buf, ihl, "TBAK", get_le32(buf + o + 36U),
                                  result);
            }
            return;
        }
        /* One frame is parsed per main-loop pass; latched commands are applied
         * before eth_rx_poll is called again, preserving transaction order. */
        if (len-o == 4U && buf[o] == 'T' && buf[o+1] == 'X' && buf[o+2] == 'S' && buf[o+3] == 'T') {
            udp_ctrl_tx_status_pending = 1;
        } else if (len-o == 4U && buf[o] == 'S' && buf[o+1] == 'N' && buf[o+2] == 'A' && buf[o+3] == 'P') {
            udp_ctrl_capture_request();
        } else if (len-o == 4U && memcmp(buf+o,"SNM1",4U)==0) {
            udp_ctrl_monitor_request();
        } else if (len-o == 8U && buf[o] == 'M' && buf[o+1] == 'O' &&
                   buf[o+2] == 'N' && buf[o+3] == 'S' && buf[o+4] == 1 &&
                   buf[o+5] <= 1 && buf[o+6] == 0 && buf[o+7] == 0) {
            udp_ctrl_monitor_source = buf[o+5];
            udp_ctrl_monitor_source_pending = 1;
        } else if (len-o == 8U && buf[o] == 'L' && buf[o+1] == 'B' &&
                   buf[o+2] == 'C' && buf[o+3] == 'K' && buf[o+4] == 1 &&
                   buf[o+5] <= 1 && buf[o+6] == 0 && buf[o+7] == 0) {
            udp_ctrl_loopback_mode = buf[o+5];
            udp_ctrl_loopback_pending = 1;
        } else if (len-o == 8U && buf[o] == 'R' && buf[o+1] == 'F' &&
                   buf[o+2] == 'R' && buf[o+3] == 'X' && buf[o+4] == 1 &&
                   buf[o+5] <= 1 && buf[o+6] == 0 && buf[o+7] == 0) {
            udp_ctrl_rf_rx_enable = buf[o+5];
            udp_ctrl_rf_rx_pending = 1;
        } else if (len-o == 20U && buf[o] == 'S' && buf[o+1] == 'Q' &&
                   buf[o+2] == 'P' && buf[o+3] == 'K' && buf[o+4] == 1 &&
                   buf[o+7] <= 1 && buf[o+5] <= 1 && buf[o+6] <= 1) {
            u32 bit_rate_word = get_le32(&buf[o+8]);
            u32 frequency_offset_word = get_le32(&buf[o+12]);
            u32 amplitude = get_le32(&buf[o+16]);
            if (amplitude <= 0x7FFFU && (!buf[o+5] ||
                    (bit_rate_word >= IRIG_SOQPSK_MIN_RATE_WORD &&
                     bit_rate_word <= IRIG_SOQPSK_MAX_RATE_WORD))) {
                udp_ctrl_soqpsk_bit_rate_word = bit_rate_word;
                udp_ctrl_soqpsk_frequency_offset_word = frequency_offset_word;
                udp_ctrl_soqpsk_amplitude = (u16)amplitude;
                udp_ctrl_soqpsk_differential = buf[o+6];
                udp_ctrl_soqpsk_randomizer = buf[o+7];
                udp_ctrl_soqpsk_enable = buf[o+5];
                udp_ctrl_soqpsk_config_pending = 1;
            } else {
                xil_printf("[soqpsk] UDP rejected rate=0x%08x amp=%u (range=0x%08x..0x%08x)\r\n",
                           (unsigned)bit_rate_word, (unsigned)amplitude,
                           (unsigned)IRIG_SOQPSK_MIN_RATE_WORD,
                           (unsigned)IRIG_SOQPSK_MAX_RATE_WORD);
            }
        } else if (len-o == 24U && buf[o] == 'P' && buf[o+1] == 'C' &&
                   buf[o+2] == 'M' && buf[o+3] == 'F' && buf[o+4] == 2 &&
                   buf[o+7] <= 1 && buf[o+5] <= 1 && buf[o+6] == 0) {
            u32 bit_rate_word = get_le32(&buf[o+8]);
            u32 deviation_word = get_le32(&buf[o+12]);
            u32 frequency_offset_word = get_le32(&buf[o+16]);
            u32 amplitude = get_le32(&buf[o+20]);
            if (amplitude <= 0x7FFFU && (!buf[o+5] || bit_rate_word != 0U)) {
                udp_ctrl_pcm_bit_rate_word = bit_rate_word;
                udp_ctrl_pcm_deviation_word = deviation_word;
                udp_ctrl_pcm_frequency_offset_word = frequency_offset_word;
                udp_ctrl_pcm_amplitude = (u16)amplitude;
                udp_ctrl_pcm_filter_profile = buf[o+6];
                udp_ctrl_pcm_randomizer = buf[o+7];
                udp_ctrl_pcm_enable = buf[o+5];
                udp_ctrl_pcm_config_pending = 1;
            }
        } else if (o + 12 <= len && memcmp(buf + o, "REFC", 4U) == 0 &&
                   ((buf[o+4] == 1 && len - o == 12U) ||
                    (buf[o+4] == 2 && len - o == 16U))) {
            u16 reserved = (u16)buf[o+6] | ((u16)buf[o+7] << 8);
            u32 rate_hz = get_le32(&buf[o+8]);
            u8 source = buf[o+5];
            /* source 0 is the board's 40 MHz crystal; source 1 is an
             * external 10 MHz reference.  Exact-rate validation prevents a
             * mismatched clock declaration from silently corrupting all PLLs. */
            if (reserved == 0 && ((source == 0 && rate_hz == 40000000UL) ||
                                  (source == 1 && rate_hz == 10000000UL))) {
                if (refclk_inflight || udp_ctrl_refclk_config_pending) {
                    /* 同一请求重试不重置PLL、不覆盖请求方及nonce；其他请求BUSY。 */
                    if (buf[o+4] == 2 && refclk_reply_pending &&
                        memcmp(refclk_last_request, buf + o, 16U) == 0 &&
                        memcmp(refclk_reply_frame + 26, buf + 26, 4U) == 0 &&
                        get_be16(refclk_reply_frame + 14U + refclk_reply_ihl) ==
                        get_be16(buf + 14U + ihl)) return;
                    if (buf[o+4] == 2)
                        udp_control_reply(buf, ihl, "RFAK", get_le32(buf + o + 12U), 3U);
                    return;
                }
                if (buf[o+4] == 2 && refclk_reply_done &&
                    memcmp(refclk_last_request, buf + o, 16U) == 0 &&
                    memcmp(refclk_reply_frame + 26, buf + 26, 4U) == 0 &&
                    get_be16(refclk_reply_frame + 14U + refclk_reply_ihl) ==
                    get_be16(buf + 14U + ihl)) {
                    udp_control_reply(buf, ihl, "RFAK", get_le32(buf + o + 12U),
                                      refclk_reply_result);
                    return;
                }
                refclk_reply_done = 0U;
                refclk_reply_pending = (buf[o+4] == 2);
                if (refclk_reply_pending) {
                    memcpy(refclk_reply_frame, buf, 14U + ihl + 8U);
                    memcpy(refclk_last_request, buf + o, 16U);
                    refclk_reply_ihl = ihl;
                }
                udp_ctrl_refclk_source = source;
                udp_ctrl_refclk_rate_hz = rate_hz;
                udp_ctrl_refclk_config_pending = 1;
            } else if (buf[o+4] == 2) {
                udp_control_reply(buf, ihl, "RFAK", get_le32(buf + o + 12U), 1U);
            }
        } else if (len-o == 28U && buf[o] == 'T' && buf[o+1] == 'X' &&
                   buf[o+2] == 'C' && buf[o+3] == 'F' &&
                   buf[o+4] <= 6 && buf[o+5] <= 6 &&
                   !(((buf[o+4] >= 3 && buf[o+4] <= 5) &&
                      (buf[o+5] >= 3 && buf[o+5] <= 5)) && buf[o+4] != buf[o+5])) {
            u16 shape = (u16)buf[o+6] | ((u16)buf[o+7] << 8);
            u8 line_code = (u8)(shape & 0x7U);
            u8 rolloff = (u8)((shape >> 4) & 0x7U);
            u64 lo_hz = get_le64(&buf[o+8]);
            u32 bw_hz = get_le32(&buf[o+16]);
            u32 att1 = get_le32(&buf[o+20]);
            u32 att2 = get_le32(&buf[o+24]);
            if (lo_hz >= 70000000ULL && lo_hz <= 6000000000ULL &&
                bw_hz >= AD9361_TX_RF_BW_MIN_HZ &&
                bw_hz <= AD9361_TX_RF_BW_MAX_HZ &&
                att1 <= 89750UL && att2 <= 89750UL &&
                line_code <= 6U && rolloff <= 5U && !(shape & ~0x77U)) {
                udp_ctrl_cfg_tx1_mode = buf[o+4];
                udp_ctrl_cfg_tx2_mode = buf[o+5];
                udp_ctrl_cfg_lo_hz = lo_hz;
                udp_ctrl_cfg_bandwidth_hz = bw_hz;
                udp_ctrl_cfg_tx1_atten_mdB = att1;
                udp_ctrl_cfg_tx2_atten_mdB = att2;
                udp_ctrl_cfg_line_code = line_code;
                udp_ctrl_cfg_rolloff = rolloff;
                udp_ctrl_tx_config_pending = 1;
            }
        } else if (len-o == 5U && buf[o] == 'T' && buf[o+1] == 'X' && buf[o+2] == 'M' &&
                   buf[o+3] <= 6 && buf[o+4] <= 6 &&
                   !(((buf[o+3] >= 3 && buf[o+3] <= 5) &&
                      (buf[o+4] >= 3 && buf[o+4] <= 5)) && buf[o+3] != buf[o+4])) {
            udp_ctrl_tx1_mode = buf[o+3];
            udp_ctrl_tx2_mode = buf[o+4];
            udp_ctrl_tx_pending = 1;
        } else if (len-o == 4U && buf[o] == 'S' && buf[o+1] == 'T' && buf[o+2] == 'R' &&
                   (buf[o+3] == '0' || buf[o+3] == '1')) {
            udp_ctrl_stream_on = (buf[o+3] == '1');
            udp_ctrl_stream_pending = 1;
        }
    }
}

void udp_stack_rx_stats(u32 *total, u32 *arp, u32 *ipv4)
{
    *total = rx_total_frames;
    *arp = rx_arp_cnt;
    *ipv4 = rx_ipv4_cnt;
}

void udp_stack_rx_reset(void)
{
    rx_total_frames = rx_arp_cnt = rx_ipv4_cnt = rx_other_cnt = 0;
    rx_arp_us = rx_icmp_us = arp_reply_tx_cnt = arp_req_tx_cnt = 0;
}

u32 udp_stack_rx_arp_us(void)   { return rx_arp_us; }
u32 udp_stack_rx_icmp_us(void)  { return rx_icmp_us; }
u32 udp_stack_arp_reply_tx(void){ return arp_reply_tx_cnt; }
u32 udp_stack_arp_req_tx(void)  { return arp_req_tx_cnt; }

u8 udp_peer_mac(u8 mac[6])
{
    int i;
    if (!peer_mac_valid) return 0;
    for (i = 0; i < 6; i++) mac[i] = peer_mac[i];
    return 1;
}

u8 udp_peer_ok(void) { return peer_mac_valid; }

u32 udp_capture_id_next(void) { return eth_capture_id++; }

/* ---- 控制口 'SNAP' 立即采集（PC UI「立即采集」按钮，串口不可用时的触发路径） ---- */
void udp_ctrl_capture_request(void)
{
    udp_ctrl_capture_size = UDP_CAPTURE_GROUPS;
    udp_ctrl_capture_req = 1;
}

void udp_ctrl_monitor_request(void)
{
    udp_ctrl_capture_size = UDP_MONITOR_GROUPS;
    udp_ctrl_capture_req = 1U;
}

u32 udp_ctrl_capture_groups(void) { return udp_ctrl_capture_size; }

u8 udp_ctrl_capture_pending(void) { return udp_ctrl_capture_req; }

void udp_ctrl_capture_clear(void)
{
    udp_ctrl_capture_req = 0;
}

u8 udp_ctrl_tx_stop_pending(void)
{
    u32 cpsr = udp_irq_save();
    u8 stopped = (udp_ctrl_tx_pending && !udp_ctrl_tx1_mode && !udp_ctrl_tx2_mode) ||
                 (udp_ctrl_tx_config_pending && !udp_ctrl_cfg_tx1_mode && !udp_ctrl_cfg_tx2_mode);
    udp_irq_restore(cpsr);
    return stopped;
}

u8 udp_ctrl_tx_mode_get(u8 *tx1_mode, u8 *tx2_mode)
{
    u32 cpsr = udp_irq_save();
    if (!udp_ctrl_tx_pending) {
        udp_irq_restore(cpsr);
        return 0;
    }
    *tx1_mode = udp_ctrl_tx1_mode;
    *tx2_mode = udp_ctrl_tx2_mode;
    udp_ctrl_tx_pending = 0;
    udp_irq_restore(cpsr);
    return 1;
}

u8 udp_ctrl_tx_config_get(u8 *tx1_mode, u8 *tx2_mode, u64 *lo_hz,
                          u32 *bandwidth_hz, u32 *tx1_atten_mdB,
                          u32 *tx2_atten_mdB, u8 *line_code,
                          u8 *rolloff)
{
    u32 cpsr = udp_irq_save();
    if (!udp_ctrl_tx_config_pending) {
        udp_irq_restore(cpsr);
        return 0;
    }
    /* 兼容旧UI紧急停发包：只关门控，不按其固定2.4GHz参数换频/产生校准音。 */
    if (!udp_ctrl_cfg_tx1_mode && !udp_ctrl_cfg_tx2_mode &&
        udp_ctrl_cfg_tx1_atten_mdB == 89750U && udp_ctrl_cfg_tx2_atten_mdB == 89750U) {
        udp_ctrl_tx_config_pending = 0U;
        udp_ctrl_tx1_mode = udp_ctrl_tx2_mode = 0U;
        udp_ctrl_tx_pending = 1U;
        udp_irq_restore(cpsr);
        return 0;
    }
    *tx1_mode = udp_ctrl_cfg_tx1_mode;
    *tx2_mode = udp_ctrl_cfg_tx2_mode;
    *lo_hz = udp_ctrl_cfg_lo_hz;
    *bandwidth_hz = udp_ctrl_cfg_bandwidth_hz;
    *tx1_atten_mdB = udp_ctrl_cfg_tx1_atten_mdB;
    *tx2_atten_mdB = udp_ctrl_cfg_tx2_atten_mdB;
    *line_code = udp_ctrl_cfg_line_code;
    *rolloff = udp_ctrl_cfg_rolloff;
    udp_ctrl_tx_config_pending = 0;
    udp_irq_restore(cpsr);
    return 1;
}

u8 udp_ctrl_refclk_config_get(u8 *source, u32 *rate_hz)
{
    u32 cpsr = udp_irq_save();
    if (!udp_ctrl_refclk_config_pending) {
        udp_irq_restore(cpsr);
        return 0;
    }
    *source = udp_ctrl_refclk_source;
    *rate_hz = udp_ctrl_refclk_rate_hz;
    udp_ctrl_refclk_config_pending = 0;
    refclk_inflight = 1U;
    udp_irq_restore(cpsr);
    return 1;
}

void udp_ctrl_refclk_complete(u32 result)
{
    refclk_inflight = 0U;
    if (!refclk_reply_pending) return;
    refclk_reply_result = result;
    refclk_reply_pending = 0U;
    refclk_reply_done = 1U;
    udp_control_reply(refclk_reply_frame, refclk_reply_ihl, "RFAK",
                      get_le32(refclk_last_request + 12U), result);
}
void udp_ctrl_refclk_invalidate(void)
{
    refclk_reply_done = 0U; /* 失锁后不再重放先前的成功ACK。 */
}

u8 udp_ctrl_pcm_config_get(u32 *bit_rate_word, u32 *deviation_word,
                           u32 *frequency_offset_word,
                           u16 *amplitude, u8 *filter_profile, u8 *randomizer,
                           u8 *enable)
{
    u32 cpsr = udp_irq_save();
    if (!udp_ctrl_pcm_config_pending) {
        udp_irq_restore(cpsr);
        return 0;
    }
    *bit_rate_word = udp_ctrl_pcm_bit_rate_word;
    *deviation_word = udp_ctrl_pcm_deviation_word;
    *frequency_offset_word = udp_ctrl_pcm_frequency_offset_word;
    *amplitude = udp_ctrl_pcm_amplitude;
    *filter_profile = udp_ctrl_pcm_filter_profile;
    *randomizer = udp_ctrl_pcm_randomizer;
    *enable = udp_ctrl_pcm_enable;
    udp_ctrl_pcm_config_pending = 0;
    udp_irq_restore(cpsr);
    return 1;
}

u8 udp_ctrl_soqpsk_config_get(u32 *bit_rate_word, u32 *frequency_offset_word,
                              u16 *amplitude, u8 *differential, u8 *randomizer,
                              u8 *enable)
{
    u32 cpsr = udp_irq_save();
    if (!udp_ctrl_soqpsk_config_pending) {
        udp_irq_restore(cpsr);
        return 0;
    }
    *bit_rate_word = udp_ctrl_soqpsk_bit_rate_word;
    *frequency_offset_word = udp_ctrl_soqpsk_frequency_offset_word;
    *amplitude = udp_ctrl_soqpsk_amplitude;
    *differential = udp_ctrl_soqpsk_differential;
    *randomizer = udp_ctrl_soqpsk_randomizer;
    *enable = udp_ctrl_soqpsk_enable;
    udp_ctrl_soqpsk_config_pending = 0;
    udp_irq_restore(cpsr);
    return 1;
}

u8 udp_ctrl_loopback_get(u8 *mode)
{
    u32 cpsr = udp_irq_save();
    if (!udp_ctrl_loopback_pending) {
        udp_irq_restore(cpsr);
        return 0;
    }
    *mode = udp_ctrl_loopback_mode;
    udp_ctrl_loopback_pending = 0;
    udp_irq_restore(cpsr);
    return 1;
}

u8 udp_ctrl_monitor_source_get(u8 *source)
{
    u32 cpsr = udp_irq_save();
    if (!udp_ctrl_monitor_source_pending) {
        udp_irq_restore(cpsr);
        return 0;
    }
    *source = udp_ctrl_monitor_source;
    udp_ctrl_monitor_source_pending = 0;
    udp_irq_restore(cpsr);
    return 1;
}

u8 udp_ctrl_rf_rx_get(u8 *enable)
{
    u32 cpsr = udp_irq_save();
    if (!udp_ctrl_rf_rx_pending) {
        udp_irq_restore(cpsr);
        return 0;
    }
    *enable = udp_ctrl_rf_rx_enable;
    udp_ctrl_rf_rx_pending = 0;
    udp_irq_restore(cpsr);
    return 1;
}

u8 udp_ctrl_stream_get(u8 *on)
{
    u32 cpsr = udp_irq_save();
    if (!udp_ctrl_stream_pending) {
        udp_irq_restore(cpsr);
        return 0;
    }
    *on = udp_ctrl_stream_on;
    udp_ctrl_stream_pending = 0;
    udp_irq_restore(cpsr);
    return 1;
}

u8 udp_ctrl_tx_status_get(void)
{
    u32 cpsr = udp_irq_save();
    u8 pending = udp_ctrl_tx_status_pending;
    udp_ctrl_tx_status_pending = 0;
    udp_irq_restore(cpsr);
    return pending;
}

/* 发一次 ARP 请求（who-has PC）。每次调用都发（主循环周期调用刷新 PC 缓存 +
 * 让 Wireshark 可观测外部 TX）；返回 0=已入 TX 环，非0=失败（链路未就绪/环满）。 */
u8 udp_arp_resolve(void)
{
    u8  *buf;
    u16 len;
    if (!peer_mac_valid) return 1U; /* Wait for a controller; no fixed boot peer. */
    buf = eth_tx_get_buf();   /* 稳定缓冲：BD 指向它，GEM DMA 异步读，栈缓冲会失效！ */
    if (udp_build_arp_request(buf, &len) != 0) return 1U;
    if (eth_tx_send(buf, len) != 0)
        return 1;
    arp_req_tx_cnt++;         /* 入环即计；若外部 TX 死，环会淤积，该计数见顶=32 */
    return 0;
}
