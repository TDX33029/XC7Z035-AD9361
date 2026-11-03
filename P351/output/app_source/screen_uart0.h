#ifndef SCREEN_UART0_H
#define SCREEN_UART0_H

#include "xil_types.h"
#include "screen_protocol.h"
#include "screen_v2.h"

/* TJC UART0基础层：STOP/APPLY在主循环执行；接收与业务执行分离。
 * APPLY由main复用受保护RF/调制/本地PN11流程，接收不等于执行成功。
 * 原诊断页面ID4/5与新页面控件冲突，默认禁用旧触摸RF映射。 */
void screen_uart0_init(void);
/* 启动/致命失败时直接在屏幕当前页画一行ASCII诊断；不依赖v2握手。 */
void screen_uart0_boot_message(const char *message);
void screen_uart0_poll(void);
/* 仅屏幕整机/本地任务拥有RF时启用；断线/重启锁存STOP，不自动恢复。 */
void screen_uart0_rf_watchdog(u8 armed);
/* 自定义A5 5A 01 02 FF FF FF停止包；在主循环执行。 */
u8 screen_uart0_take_stop(void);
/* 只查看STOP锁存，换频恢复发射前检查；不得清除待执行的STOP。 */
u8 screen_uart0_stop_pending_get(void);
/* 保留旧API链接兼容；SCREEN_LEGACY_TOUCH=0时不会生成CW请求。 */
u8 screen_uart0_take_cw(void);
void screen_uart0_set_status(const char *message);
/* 执行期间拒绝新APPLY，STOP仍无条件接收；不得覆盖进行中的事务。 */
void screen_uart0_set_busy(u8 busy);
/* 只取已校验的完整请求；RF/组帧在主循环执行，不能在UART收字节时执行。 */
u8 screen_uart0_take_apply(screen_apply_t *request);
u8 screen_uart0_take_command(screen_command_t *request);
/* v2专用全局变量回读，不覆盖屏幕编辑草稿。每次最多排一个完整TJC指令。 */
u8 screen_uart0_v2_link(void);
u8 screen_uart0_write_number(const char *name, u32 value);
void screen_uart0_ack(u32 sequence, u32 op, u32 result);
void screen_uart0_ack_get(u32 *sequence, u32 *op, u32 *result);

#endif
