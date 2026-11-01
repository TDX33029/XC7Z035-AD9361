#ifndef P351_CLOCK_H
#define P351_CLOCK_H
#include "xil_types.h"
enum {
    P351_CLOCK_INTERNAL = 0,
    P351_CLOCK_WAIT_INPUT = 1,
    P351_CLOCK_WAIT_LOCK = 2,
    P351_CLOCK_EXTERNAL = 3,
    P351_CLOCK_FAULT = 4,
    P351_CLOCK_WAIT_INTERNAL = 5,
    P351_CLOCK_TEST_LOW = 6,
    P351_CLOCK_TEST_HIGH = 7
};
void p351_clock_init(void);
/* 仅在发射/DMA已停止后调用。begin受理不等于完成；service非阻塞推进。 */
int p351_clock_begin(u8 source, u32 now_ms);
void p351_clock_service(u32 now_ms);
void p351_clock_abort(void);
u8 p351_clock_state(void);
u8 p351_clock_busy(void);
u8 p351_clock_ready(void);
u8 p351_clock_source(void);
u32 p351_clock_inputs(void);
u32 p351_adf_function_word(u8 quiet);
u32 p351_adf_r_word(void);
u32 p351_adf_n_word(void);
#endif
