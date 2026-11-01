#ifndef SCREEN_UP70_API_H
#define SCREEN_UP70_API_H
#include "final_rf_status.h"
/* 主循环与长事务中的同一联锁；不从UART中断调用。 */
static u8 screen_up70_guard(void);
static u8 screen_up70_interlock(u64 lo_hz,u32 rate,u32 attenuation,u32 external);
static void screen_up70_output_stage(u32 external);
static u8 screen_up70_chip_healthy(u32 expected_atten);
int board_final_rf_owned(void);
#endif
