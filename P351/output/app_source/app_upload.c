/* ============================================================================
 * app_upload.c —— 流式 UDP 上传
 *
 * P351 V6：DMA采集131072个真实复样本（512 KiB）→820个UDP分片。
 * 最后分片32组；旧协议空槽在PS补零，不占用DMA带宽。
 * → UDP 40001 发出。捕获路径零串口打印（避免 UART 洪水），每 1000 次打状态。
 *
 * DMA字序：rx_buf[k]={Q0,I0}，物理RX2映射逻辑槽0，12bit符号扩展。
 * UDP载荷：int16 LE [i0 q0 0 0]×n_groups，仍兼容ZSDR v1。
 * 单次2.133 ms采样块连续；不同SNAP之间有空隙，不能拼成连续波形。
 *
 * 使能源：UDP 控制口的 STR1/STR0 命令。导轨/EMIO 触发已移除。
 * ==========================================================================*/
#include "app_upload.h"
#include "dma_rx.h"
#include "eth_link.h"
#include "udp_stack.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "sleep.h"
#include "screen_uart0.h"

/* 仅收串口/锁存STOP，不在上传或DMA回调内执行RF业务。 */
static u8 udp_stream_control_yield(void)
{
    screen_uart0_poll();
    return screen_uart0_stop_pending_get();
}

/* ---- main.c 的采集缓冲（static，此处 extern 引用） ---- */
extern u32 rx_buf[UDP_CAPTURE_WORDS];

/* ---- 流式状态 ---- */
static u8  stream_enabled   = 0;
static u8  stream_active    = 0;
static u32 captures_done    = 0;
static u32 stream_err       = 0;
static u32 frame_buf_idx    = 0;   /* TX 缓冲轮转 */
/* 只记录实际DDR数据，不用DDS模式/valid标志代替波形证据。 */
static u32 iq_groups, iq_capture_id, iq_source, iq_nonzero, iq_changes;
static s16 iq_i_min, iq_i_max, iq_q_min, iq_q_max;

static void udp_stream_iq_measure(u32 groups, u32 capture_id)
{
    u32 k, previous = rx_buf[0];
    iq_groups=groups; iq_capture_id=capture_id; iq_source=rx2r_get_source();
    iq_nonzero=iq_changes=0U;
    iq_i_min=iq_i_max=(s16)previous;
    iq_q_min=iq_q_max=(s16)(previous>>16);
    for(k=0U;k<groups;k++) {
        if ((k & 255U) == 0U) (void)udp_stream_control_yield();
        u32 word=rx_buf[k];
        s16 i=(s16)word, q=(s16)(word>>16);
        if(word) iq_nonzero++;
        if(k && word!=previous) iq_changes++;
        if(i<iq_i_min) iq_i_min=i;
        if(i>iq_i_max) iq_i_max=i;
        if(q<iq_q_min) iq_q_min=q;
        if(q>iq_q_max) iq_q_max=q;
        previous=word;
    }
}

void udp_stream_iq_print(void)
{
    if(!iq_groups) {
        xil_printf("[tx-check] IQ WAIT no valid DMA capture; MON tx_seen is not waveform proof\r\n");
        return;
    }
    xil_printf("[tx-check] IQ cap=%u source=%s groups=%u nonzero=%u changes=%u I=%d..%d Q=%d..%d (last capture, pre-LVDS, not RF proof)\r\n",
        (unsigned)iq_capture_id,iq_source?"TX_DAC":"RX_ADC",(unsigned)iq_groups,
        (unsigned)iq_nonzero,(unsigned)iq_changes,(int)iq_i_min,(int)iq_i_max,(int)iq_q_min,(int)iq_q_max);
}

void udp_stream_force(u8 on)
{
    stream_enabled = on ? 1 : 0;
    xil_printf("[udp] force=%u\r\n", (unsigned)on);
}

void udp_stream_init(void)
{
    udp_stack_init();
    captures_done = 0;
    stream_err = 0;
    frame_buf_idx = 0;
    stream_active = 0;
    stream_enabled = 0;
    iq_groups = 0U;
    xil_printf("[udp] stream controlled by UDP STR1/STR0\r\n");
}

u8 udp_stream_active(void) { return stream_active; }
u32 udp_stream_captures(void) { return captures_done; }   /* 已成功完成的采集数（auto 回退判成功） */
u32 udp_stream_errors(void) { return stream_err; }

/* 一次采集 + 拆帧上传 */
void udp_stream_service(void)
{
    u32 i, g, ng, fcnt;
    u32 ts_lo, ts_hi;
    zsdr_hdr_t hdr;
    s16 payload[UDP_N_G_PER_FRAME * 4];
    u8 *fbuf;
    u8 peer[6];
    u16 len;
    u32 cap_id;
    u32 retry;
    u8 capture_sent_ok = 1U;
    u32 capture_groups = UDP_CAPTURE_GROUPS;
    int capture_result;

    if (udp_stream_control_yield()) return;

    /* ---- 远程立即采集（UDP 40000 'SNAP'，PC UI「立即采集」按钮）----
     * 一次一采，不依赖使能/DWELL：就绪（链路 up + peer 已解析）才消费；
     * 未就绪则保持 pending 并主动 ARP，主循环周期 ARP 收敛后下次即采。 */
    if (udp_ctrl_capture_pending()) {
        if (!eth_link_is_up() || !udp_peer_mac(peer)) {
            udp_arp_resolve();
            return;
        }
        capture_groups = udp_ctrl_capture_groups();
        udp_ctrl_capture_clear();
        goto do_capture;
    }

    if (!stream_enabled) { stream_active = 0; return; }
    stream_active = 1;

do_capture:

    /* ---- 链路/对端未就绪：发 ARP 并等待（不阻塞主循环） ---- */
    if (!eth_link_is_up())
        return;
    if (!udp_peer_mac(peer)) {
        udp_arp_resolve();
        return;
    }

    /* ---- 采集（静默路径） ---- */
    if (capture_groups != UDP_MONITOR_GROUPS && capture_groups != UDP_CAPTURE_GROUPS) {
        stream_err++;
        return;
    }
    if (dma_rx_start(rx_buf, capture_groups) != XST_SUCCESS) {
        stream_err++;
        return;
    }
    rx2r_trigger(capture_groups);
    /* 最长正常块为2.133ms；100ms真实超时，避免坏采集长期阻塞控制主循环。 */
    dma_rx_set_wait_hook(udp_stream_control_yield);
    capture_result = dma_rx_wait_done(100000U);
    dma_rx_set_wait_hook(0);
    if (capture_result != XST_SUCCESS) {
        if (!screen_uart0_stop_pending_get()) stream_err++;
        return;
    }
    Xil_DCacheInvalidateRange((UINTPTR)rx_buf, capture_groups * 4u);
    /* 背压虽然保持AXIS字，但会跳过期间的新ADC样本；不能把这种块当成
     * 连续采样做解调/BER。整个块丢弃，并保留明确错误计数。 */
    if (rx2r_status() & RX2R_ST_BP) {
        stream_err++;
        return;
    }
    rx2r_read_ts(&ts_lo, &ts_hi);

    cap_id = udp_capture_id_next();
    udp_stream_iq_measure(capture_groups, cap_id);

    /* ---- 拆帧：131072组 → 820分片（819×160 + 32） ---- */
    fcnt = (capture_groups + UDP_N_G_PER_FRAME - 1) / UDP_N_G_PER_FRAME;
    for (i = 0; i < fcnt; i++) {
        if (udp_stream_control_yield()) return; /* 不等820个分片发完才响应STOP。 */
        ng = UDP_N_G_PER_FRAME;
        if ((i + 1) * UDP_N_G_PER_FRAME > capture_groups)
            ng = capture_groups - i * UDP_N_G_PER_FRAME;
        /* 打包 8B/组 */
        for (g = 0; g < ng; g++) {
            u32 w0 = rx_buf[i * UDP_N_G_PER_FRAME + g];
            u32 w1 = 0U; /* 物理RX2在逻辑槽0；兼容UDP空槽由PS补，不占DMA。 */
            payload[4 * g + 0] = (s16)(w0 & 0xFFFF);        /* i0 */
            payload[4 * g + 1] = (s16)((w0 >> 16) & 0xFFFF);/* q0 */
            payload[4 * g + 2] = (s16)(w1 & 0xFFFF);        /* i1 */
            payload[4 * g + 3] = (s16)((w1 >> 16) & 0xFFFF);/* q1 */
        }
        /* ZSDR 头 */
        hdr.magic[0]='Z'; hdr.magic[1]='S'; hdr.magic[2]='D'; hdr.magic[3]='R';
        hdr.ver  = 1;
        hdr.flags = 0;
        if (i == fcnt - 1) hdr.flags |= 0x01;    /* END */
        hdr.pkt_len = (u16)(ZSDR_HDR_LEN + ng * 8);
        hdr.capture_id = cap_id;
        hdr.frame_index = (u16)i;
        hdr.frame_count = (u16)fcnt;
        hdr.n_groups    = (u16)ng;
        hdr.rsvd0       = 0;
        hdr.ts_lo       = ts_lo;
        hdr.ts_hi       = ts_hi;
        hdr.rsvd1       = 0;

        /* 每帧发送前给 TX 环留两个槽位给 ARP/状态帧。旧代码
         * 26 帧连发时一旦 BD 短暂耗尽就直接丢帧，PC 端永远等不到
         * 完整 capture，表现为图像页停止。这里仅做毫秒级有界等待，
         * 不会把主循环无限卡住。 */
        for (retry = 0U;
             eth_tx_pending() >= (TX_BD_CNT - 2U) && retry < 500U;
             retry++) {
            if (udp_stream_control_yield()) return;
            usleep(10U);
        }
        if (eth_tx_pending() >= (TX_BD_CNT - 2U)) {
            stream_err++;
            capture_sent_ok = 0U;
            break;
        }

        /* TX 缓冲：64 个轮转缓冲 > 32 BD，不覆盖在途数据。 */
        fbuf = eth_tx_get_buf();
        udp_build_zsdr_frame(fbuf, &len, &hdr, payload, (u16)(ng * 8));
        for (retry = 0U; retry < 200U; retry++) {
            if (udp_stream_control_yield()) return;
            if (eth_tx_send(fbuf, len) == 0)
                break;
            usleep(10U);
        }
        if (retry == 200U) {
            stream_err++;
            capture_sent_ok = 0U;
            break;
        }
        frame_buf_idx++;
    }

    if (capture_sent_ok)
        captures_done++;
    if ((captures_done % UDP_STATUS_EVERY) == 0) {
        xil_printf("[udp] cap=%u err=%u ts=%u.%u\r\n",
                (unsigned)captures_done, (unsigned)stream_err,
                (unsigned)ts_hi, (unsigned)ts_lo);
    }
    usleep(UDP_PACE_US);
}
