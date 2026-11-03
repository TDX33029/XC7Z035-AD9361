#include "screen_uart0.h"
#include "xparameters.h"

/* 屏幕独占UART0。防止Vitis新建BSP的默认控制台污染TJC指令流。
 * 应在BSP Settings将stdin/stdout设为ps7_uart_1后重新生成平台，
 * 不能只改xparameters.h或忽略编译错误。主机模拟未定义这些宏时不参与。 */
#if defined(STDOUT_BASEADDRESS) && defined(XPAR_PS7_UART_0_BASEADDR)
#if STDOUT_BASEADDRESS == XPAR_PS7_UART_0_BASEADDR
#error "P351 BSP stdout conflicts with screen UART0; select ps7_uart_1 and regenerate BSP"
#endif
#endif
#if defined(STDIN_BASEADDRESS) && defined(XPAR_PS7_UART_0_BASEADDR)
#if STDIN_BASEADDRESS == XPAR_PS7_UART_0_BASEADDR
#error "P351 BSP stdin conflicts with screen UART0; select ps7_uart_1 and regenerate BSP"
#endif
#endif

/* The generic XPAR_XUARTPS_0_DEVICE_ID can denote UART1 when UART0 is absent.
 * Only the peripheral-specific macro proves that the new UART0 BSP exists. */
#ifdef XPAR_PS7_UART_0_DEVICE_ID
#include "xuartps.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include <string.h>
#include <stdio.h>

#define SCREEN_RX_LIMIT 64U
#define SCREEN_TX_LIMIT 32U
#define SCREEN_POLL_RX_BUDGET 32U
#define SCREEN_POLL_TX_BUDGET 4U

static XUartPs screen_uart;
static u8 screen_ready;
static u8 screen_link;
static u8 screen_page;
static u8 screen_rx[SCREEN_RX_LIMIT];
static u8 screen_rx_len;
static u8 screen_tx[SCREEN_TX_LIMIT];
static u8 screen_tx_len;
static u8 screen_tx_pos;
static XTime screen_last_probe;
static XTime screen_last_reply;
static u32 screen_touch_count;
static u32 screen_bad_frames;
static u32 screen_boot_count;
static u8 screen_stop_pending;
static u8 screen_rf_watchdog;
static u8 screen_cw_pending;
static u8 screen_cw_armed;
static u8 screen_cw_down;
static XTime screen_cw_arm_time;
static XTime screen_cw_press_time;
static XTime screen_last_rx_byte;
static u8 screen_text_pending;
static char screen_text[11];
static screen_apply_t screen_apply;
static u8 screen_apply_pending;
static u8 screen_busy;
static u32 screen_sequence;
static u8 screen_stop_match;
static screen_command_t screen_command;
static u8 screen_command_pending, screen_v2_active;
static u32 screen_ack_sequence, screen_ack_op, screen_ack_result;
static u32 screen_command_result;
static void screen_enqueue(const char *command);

void screen_uart0_rf_watchdog(u8 armed)
{
    screen_rf_watchdog=armed ? 1U : 0U;
    if(screen_rf_watchdog && (!screen_ready || !screen_link))
        screen_stop_pending=1U;
}

void screen_uart0_ack(u32 seq, u32 op, u32 rc)
{
    screen_ack_sequence=seq; screen_ack_op=op; screen_ack_result=rc;
    if(seq==screen_command.sequence && op==screen_command.op)
        screen_command_result=rc;
}
void screen_uart0_ack_get(u32 *seq, u32 *op, u32 *rc)
{
    *seq=screen_ack_sequence; *op=screen_ack_op; *rc=screen_ack_result;
}
u8 screen_uart0_v2_link(void) { return screen_v2_active && screen_link; }

u8 screen_uart0_write_number(const char *name, u32 value)
{
    char command[32];
    u32 i;
    if(!screen_uart0_v2_link() || screen_tx_len || !name) return 0;
    /* qualified为9字母；长度限制必须与完整快照字段一致，否则会永远停在该字段。
     * 12+1+10+3=26，仍小于32字节发送缓冲。 */
    for(i=0;name[i];++i) if(i>=12 || name[i]<'a' || name[i]>'z') return 0;
    (void)snprintf(command,sizeof command,"%s=%lu",name,(unsigned long)value);
    /* 同一主循环中没有并发写者；下一轮UART预算化发送。 */
    screen_enqueue(command);
    return screen_tx_len!=0;
}

void screen_uart0_set_busy(u8 busy) { screen_busy = busy ? 1U : 0U; }

void screen_uart0_set_status(const char *message)
{
    u32 i;
    if (!message)
        return;
    for (i = 0U; i < 10U && message[i]; ++i) {
        if (message[i] < ' ' || message[i] > '~' ||
            message[i] == '"' || message[i] == '\\')
            return;
    }
    if (message[i])
        return;
    memcpy(screen_text, message, i);
    screen_text[i] = '\0';
    screen_text_pending = 1U;
}

static void screen_enqueue(const char *command)
{
    u32 n = (u32)strlen(command);
    if (screen_tx_pos != screen_tx_len || n + 3U > SCREEN_TX_LIMIT)
        return; /* No blocking or interleaved TJC instructions. */
    memcpy(screen_tx, command, n);
    screen_tx[n] = screen_tx[n + 1U] = screen_tx[n + 2U] = 0xffU;
    screen_tx_pos = 0U;
    screen_tx_len = (u8)(n + 3U);
}

static void screen_complete_frame(void)
{
    if (screen_rx[0] == 0xA5U) {
        screen_apply_t candidate;
        if (screen_rx[2] >= 2U && screen_rx[2] <= 6U) {
            screen_command_t cmd;
            if (!screen_command_decode(screen_rx,screen_rx_len,&cmd)) {
                ++screen_bad_frames;
            } else {
                screen_v2_active=1;
                if (!screen_link || screen_stop_pending) {
                    screen_uart0_ack(cmd.sequence,cmd.op,SC_NOT_READY);
                } else if (cmd.sequence==screen_command.sequence) {
                    if (memcmp(&cmd,&screen_command,sizeof cmd)) {
                        u32 saved=screen_command_result;
                        screen_uart0_ack(cmd.sequence,cmd.op,SC_SEQUENCE);
                        screen_command_result=saved;
                    } else screen_uart0_ack(cmd.sequence,cmd.op,screen_command_result);
                    /* 完全相同的重发仅保留/回读ACK，不再次执行动作。 */
                } else if (screen_busy || screen_command_pending || screen_apply_pending) {
                    screen_uart0_ack(cmd.sequence,cmd.op,SC_BUSY);
                } else {
                    screen_command=cmd; screen_command_pending=1;
                    screen_uart0_ack(cmd.sequence,cmd.op,SC_PENDING);
                }
            }
        } else if (screen_rx[3] == 2U) {
            screen_stop_pending = 1U;
            screen_apply_pending = screen_cw_pending = screen_cw_armed = 0U;
        } else if (screen_busy || screen_command_pending) {
            screen_uart0_set_status("BUSY");
        } else if (screen_stop_pending || !screen_link || screen_page != 0U) {
            screen_uart0_set_status("NOT READY");
        } else if (!screen_apply_decode(screen_rx, screen_rx_len, &candidate)) {
            ++screen_bad_frames;
            screen_uart0_set_status("BAD PARAM");
        } else if (candidate.sequence == screen_sequence) {
            /* 序号相同但参数不同必须拒绝，不能把新请求冒充已执行的重发。
             * screen_apply保留最近一次接受的完整内容，主循环取走后也不清除。 */
            if (memcmp(&candidate, &screen_apply, sizeof(candidate)) != 0)
                screen_uart0_set_status("SEQ ERROR");
        } else if (!screen_apply_pending) {
            screen_apply = candidate; /* 后续包不得覆盖主循环尚未取走的请求。 */
            screen_sequence = screen_apply.sequence;
            screen_apply_pending = 1U;
            screen_uart0_set_status("WAIT PS");
        } else {
            screen_uart0_set_status("BUSY");
        }
    } else if (screen_rx[0] == 0x66U) {
        screen_page = screen_rx[1];
        if (screen_page != 0U) {
            screen_cw_armed = screen_cw_down = screen_cw_pending = 0U;
            /* 请求尚未交主循环执行，取消后允许相同序号重发。 */
            if (screen_apply_pending) screen_sequence = 0U;
            screen_apply_pending = 0U;
        }
        XTime_GetTime(&screen_last_reply);
        if (!screen_link) {
            screen_link = 1U;
            screen_uart0_set_status("LINK OK");
            xil_printf("[screen] UART0 TJC link detected, page=%u; IF v4 / RF request v5, legacy writes disabled\r\n",
                       (unsigned)screen_rx[1]);
        }
    } else if (screen_rx[0] == 0x65U) {
        /* Only the frozen page0 component IDs below may control RF.
         * A CW tap must contain both press and release; two complete taps
         * are required, so a duplicated release frame cannot enable RF. */
        ++screen_touch_count;
        if (SCREEN_LEGACY_TOUCH && screen_rx[1] == 0U && screen_rx[2] == 4U &&
            screen_rx[3] == 1U) {
            screen_stop_pending = 1U;
            screen_cw_pending = 0U;
            screen_cw_armed = 0U;
            screen_cw_down = 0U;
        } else if (SCREEN_LEGACY_TOUCH && screen_link && screen_page == 0U && !screen_stop_pending && screen_rx[1] == 0U &&
                   screen_rx[2] == 5U && screen_rx[3] == 1U) {
            XTime_GetTime(&screen_cw_press_time);
            screen_cw_down = 1U;
        } else if (SCREEN_LEGACY_TOUCH && screen_link && screen_page == 0U && !screen_stop_pending && screen_cw_down &&
                   screen_rx[1] == 0U && screen_rx[2] == 5U &&
                   screen_rx[3] == 0U) {
            XTime now;
            screen_cw_down = 0U;
            XTime_GetTime(&now);
            if (now - screen_cw_press_time > (XTime)COUNTS_PER_SECOND * 2U) {
                screen_cw_armed = 0U;
                screen_rx_len = 0U;
                return;
            }
            if (screen_cw_armed &&
                now - screen_cw_arm_time >= (XTime)COUNTS_PER_SECOND / 10U &&
                now - screen_cw_arm_time <= (XTime)COUNTS_PER_SECOND * 3U) {
                screen_cw_pending = 1U;
                screen_cw_armed = 0U;
                screen_uart0_set_status("CW APPLY");
            } else {
                screen_cw_armed = 1U;
                screen_cw_arm_time = now;
                screen_uart0_set_status("CW AGAIN");
            }
        }
    } else if (screen_rx[0] == 0x88U) {
        if(screen_rf_watchdog) screen_stop_pending=1U;
        /* The screen has rebooted. Do not retain its former page/link state. */
        screen_link = 0U;
        screen_text_pending = 0U;
        screen_cw_armed = 0U;
        screen_cw_down = 0U;
        screen_cw_pending = 0U;
        screen_apply_pending = 0U;
        screen_sequence = 0U;
        screen_command_pending=screen_v2_active=0;
        memset(&screen_command,0,sizeof screen_command);
        ++screen_boot_count;
        screen_last_probe = 0U;
        xil_printf("[screen] restart #%u; awaiting fresh page reply, screen-owned RF STOP=%u\r\n",
                   (unsigned)screen_boot_count,(unsigned)screen_rf_watchdog);
    }
    screen_rx_len = 0U;
}

static void screen_rx_byte(u8 byte)
{
    static const u8 stop_frame[7] = {0xA5U, 0x5AU, 1U, 2U, 255U, 255U, 255U};
    u32 expected = 0U;
    XTime_GetTime(&screen_last_rx_byte);
    /* STOP独立识别，避免前一条APPLY半包把紧随其后的急停吞掉。
     * 本v1/v2/v3/v4/v5协议所有合法字段及校验最高字节为0，合法APPLY内部
     * 不可能出现此7字节STOP串。以后扩展字段范围/版本须重新审查此约束。 */
    screen_stop_match = byte == stop_frame[screen_stop_match] ?
                        (u8)(screen_stop_match + 1U) : (byte == 0xA5U ? 1U : 0U);
    if (screen_stop_match == sizeof(stop_frame)) {
        screen_stop_match = screen_rx_len = 0U;
        screen_stop_pending = 1U;
        screen_apply_pending = screen_cw_pending = screen_cw_armed = screen_cw_down = 0U;
        if(screen_command_pending) {
            screen_uart0_ack(screen_command.sequence,screen_command.op,SC_STOPPED);
            screen_command_pending=0;
        }
        return;
    }
    if (!screen_rx_len) {
        if (byte != 0xA5U && byte != 0x65U && byte != 0x66U && byte != 0x70U &&
            byte != 0x88U &&
            byte != 0x71U && byte > 0x24U) {
            ++screen_bad_frames;
            return;
        }
    }
    if (screen_rx_len >= SCREEN_RX_LIMIT) {
        ++screen_bad_frames;
        screen_rx_len = 0U;
        return;
    }
    screen_rx[screen_rx_len++] = byte;
    switch (screen_rx[0]) {
    case 0xA5U:
        if (screen_rx_len >= 2U && screen_rx[1] != 0x5AU) goto bad_frame;
        if (screen_rx_len < 4U) break;
        if (screen_rx[2]==2U) expected=SCREEN_APPLY_BYTES;
        else if (screen_rx[2]>=3U && screen_rx[2]<=6U) expected=63U;
        else {
            if (screen_rx[2] != 1U || (screen_rx[3] != 1U && screen_rx[3] != 2U)) goto bad_frame;
            expected = screen_rx[3] == 1U ? SCREEN_APPLY_BYTES : 7U;
        }
        break;
    case 0x65U: expected = 7U; break;  /* touch event */
    case 0x66U: expected = 5U; break;  /* page returned by sendme */
    case 0x71U: expected = 8U; break;  /* numeric: four LE bytes may contain FF */
    case 0x70U: /* bounded, FF-terminated text */
        if (screen_rx_len >= 4U &&
            screen_rx[screen_rx_len - 1U] == 0xffU &&
            screen_rx[screen_rx_len - 2U] == 0xffU &&
            screen_rx[screen_rx_len - 3U] == 0xffU)
            expected = screen_rx_len;
        break;
    default: expected = 4U; break;  /* command result/error */
    }
    if (!expected || screen_rx_len < expected)
        return;
    if (screen_rx[expected - 1U] == 0xffU &&
        screen_rx[expected - 2U] == 0xffU &&
        screen_rx[expected - 3U] == 0xffU)
        screen_complete_frame();
    else {
bad_frame:
        ++screen_bad_frames;
        screen_rx_len = 0U;
    }
}

void screen_uart0_init(void)
{
    XUartPs_Config *cfg = XUartPs_LookupConfig(XPAR_PS7_UART_0_DEVICE_ID);
    XUartPsFormat format = {115200U, XUARTPS_FORMAT_8_BITS,
                           XUARTPS_FORMAT_NO_PARITY, XUARTPS_FORMAT_1_STOP_BIT};
    screen_ready = screen_link = screen_rf_watchdog = 0U;
    screen_rx_len = screen_tx_len = screen_tx_pos = 0U;
    screen_stop_pending = screen_cw_pending = screen_cw_armed = screen_cw_down = 0U;
    screen_text_pending = 0U;
    screen_apply_pending = screen_busy = 0U;
    screen_sequence = 0U;
    screen_stop_match = 0U;
    screen_command_pending=screen_v2_active=0;
    memset(&screen_command,0,sizeof screen_command);
    screen_ack_sequence=screen_ack_op=screen_ack_result=0;
    screen_page = 0xffU;
    if (!cfg || cfg->BaseAddress != 0xE0000000U ||
        XUartPs_CfgInitialize(&screen_uart, cfg, cfg->BaseAddress) != XST_SUCCESS ||
        XUartPs_SetDataFormat(&screen_uart, &format) != XST_SUCCESS) {
        xil_printf("[screen] UART0 init failed; screen disabled\r\n");
        return;
    }
    XUartPs_SetOperMode(&screen_uart, XUARTPS_OPER_MODE_NORMAL);
    screen_ready = 1U;
    XTime_GetTime(&screen_last_probe);
    screen_last_reply = screen_last_probe;
    screen_enqueue("bkcmd=3");
    xil_printf("[screen] UART0 115200 8N1 ready; IF v4 / RF request v5, legacy writes disabled\r\n");
}

/* 启动早期或tx_boot_fail直接画一行ASCII，绕开尚未启动的v2握手。
 * 只用于诊断，不写业务变量，不参与RF/所有权判断。 */
void screen_uart0_boot_message(const char *message)
{
    char command[128];
    char safe[64];
    u32 i, n;
    if (!message || !screen_ready)
        return;
    for (i = 0U; i + 1U < sizeof safe && message[i]; ++i) {
        char c = message[i];
        safe[i] = (c >= ' ' && c <= '~' && c != '"' && c != '\\') ? c : '?';
    }
    safe[i] = '\0';
    n = (u32)snprintf(command, sizeof command,
                      "xstr 8,112,464,24,5,63488,65535,0,1,1,\"P351: %s\"", safe);
    if (n >= sizeof command)
        n = sizeof command - 1U;
    for (i = 0U; i < n; ++i)
        XUartPs_SendByte(screen_uart.Config.BaseAddress, (u8)command[i]);
    XUartPs_SendByte(screen_uart.Config.BaseAddress, 0xFFU);
    XUartPs_SendByte(screen_uart.Config.BaseAddress, 0xFFU);
    XUartPs_SendByte(screen_uart.Config.BaseAddress, 0xFFU);
}

void screen_uart0_poll(void)
{
    XTime now;
    u32 i;
    if (!screen_ready)
        return;
    XTime_GetTime(&now);
    if (screen_rx_len && now - screen_last_rx_byte >
        (XTime)COUNTS_PER_SECOND / 5U) {
        screen_rx_len = 0U; /* Discard a stalled partial frame. */
        screen_stop_match = 0U;
        ++screen_bad_frames;
    }
    for (i = 0U; i < SCREEN_POLL_RX_BUDGET &&
         XUartPs_IsReceiveData(screen_uart.Config.BaseAddress); ++i)
        screen_rx_byte((u8)XUartPs_ReadReg(screen_uart.Config.BaseAddress,
                                            XUARTPS_FIFO_OFFSET));
    for (i = 0U; i < SCREEN_POLL_TX_BUDGET && screen_tx_pos < screen_tx_len; ++i) {
        if (XUartPs_ReadReg(screen_uart.Config.BaseAddress, XUARTPS_SR_OFFSET) &
            XUARTPS_SR_TXFULL)
            break;
        XUartPs_WriteReg(screen_uart.Config.BaseAddress, XUARTPS_FIFO_OFFSET,
                         screen_tx[screen_tx_pos++]);
    }
    if (screen_tx_pos == screen_tx_len)
        screen_tx_pos = screen_tx_len = 0U;
    XTime_GetTime(&now);
    if (now - screen_last_probe >= (XTime)COUNTS_PER_SECOND * 2U && !screen_tx_len) {
        screen_enqueue("sendme");
        screen_last_probe = now;
    } else if (screen_link && screen_page == 0U && screen_text_pending && !screen_tx_len) {
        char command[32];
        strcpy(command, "shouye.t5.txt=\"");
        strcat(command, screen_text);
        strcat(command, "\"");
        screen_enqueue(command);
        screen_text_pending = 0U;
    }
    if (screen_link && now - screen_last_reply >= (XTime)COUNTS_PER_SECOND * 6U) {
        if(screen_rf_watchdog) screen_stop_pending=1U;
        screen_link = 0U;
        screen_cw_armed = 0U;
        screen_cw_down = 0U;
        screen_cw_pending = 0U;
        if (screen_apply_pending) screen_sequence = 0U;
        screen_apply_pending = 0U;
        if(screen_command_pending) {
            screen_uart0_ack(screen_command.sequence,screen_command.op,SC_NOT_READY);
            memset(&screen_command,0,sizeof screen_command);
            screen_command_pending=0;
        }
        screen_uart0_set_status("LINK LOST");
        xil_printf("[screen] link timeout; screen-owned RF STOP=%u, touch=%u bad=%u\r\n",
                   (unsigned)screen_rf_watchdog,(unsigned)screen_touch_count,(unsigned)screen_bad_frames);
    }
}

u8 screen_uart0_stop_pending_get(void) { return screen_stop_pending; }

u8 screen_uart0_take_stop(void)
{
    u8 pending = screen_stop_pending;
    screen_stop_pending = 0U;
    return pending;
}

u8 screen_uart0_take_cw(void)
{
    u8 pending = screen_cw_pending;
    screen_cw_pending = 0U;
    return pending;
}

u8 screen_uart0_take_apply(screen_apply_t *request)
{
    if (!request || !screen_apply_pending || screen_stop_pending) return 0U;
    *request = screen_apply;
    screen_apply_pending = 0U;
    return 1U;
}

u8 screen_uart0_take_command(screen_command_t *request)
{
    if(!request || !screen_command_pending || screen_stop_pending) return 0;
    *request=screen_command; screen_command_pending=0;
    return 1;
}

#else
void screen_uart0_init(void) {}
void screen_uart0_boot_message(const char *message) { (void)message; }
void screen_uart0_rf_watchdog(u8 armed) { (void)armed; }
void screen_uart0_poll(void) {}
u8 screen_uart0_take_stop(void) { return 0U; }
u8 screen_uart0_stop_pending_get(void) { return 0U; }
u8 screen_uart0_take_cw(void) { return 0U; }
void screen_uart0_set_status(const char *message) { (void)message; }
void screen_uart0_set_busy(u8 busy) { (void)busy; }
u8 screen_uart0_take_apply(screen_apply_t *request) { (void)request; return 0U; }
u8 screen_uart0_take_command(screen_command_t *request) { (void)request; return 0U; }
u8 screen_uart0_v2_link(void) { return 0; }
u8 screen_uart0_write_number(const char *name,u32 value) { (void)name;(void)value;return 0; }
void screen_uart0_ack(u32 s,u32 op,u32 rc) { (void)s;(void)op;(void)rc; }
void screen_uart0_ack_get(u32 *s,u32 *op,u32 *rc) { *s=*op=*rc=0; }
#endif
