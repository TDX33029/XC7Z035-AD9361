#ifndef FINAL_RF_STATUS_H
#define FINAL_RF_STATUS_H
#include "xil_types.h"
/* FRFQ+nonce -> FRFA+nonce+32个小端u32。只读，不使用SPI事务或改变目的地址。
 * CFGA V9的38字保持不变。屏幕与网口从同一PS状态构造快照，允许交错读取。
 * RF有效位只代表数字执行状态；目标/标定估算不是在线射频功率测量。 */
#define FINAL_RF_STATUS_VERSION 1U
#define FINAL_RF_STATUS_WORDS 32U
#define FR_VALID 1U
#define FR_OWNED 2U
#define FR_QUALIFIED 4U
#define FR_COMMITTED 8U
#define FR_REFERENCE_OK 16U
#define FR_TFS_LOCK 32U
#define FR_PLAN_KNOWN 64U
#define FR_MANUAL_TEST 128U
#define FR_DIRECT_ACTIVE 256U
enum {
    FR_VERSION, FR_FLAGS, FR_PHASE, FR_FAULT, FR_FINAL_KHZ, FR_TARGET_MDBM,
    FR_EXPECT_MDBM, FR_MODEL, FR_IF_KHZ, FR_IF_TARGET_MDBM, FR_IF_ATT_MDB,
    FR_EXT_HALFDB, FR_LO2_KHZ, FR_TFS_STATUS, FR_MODE, FR_RATE, FR_CODE,
    FR_FRAMED, FR_FBYTES, FR_FCOUNT, FR_SYNC_LOW, FR_SYNC_HIGH, FR_SYNC_BITS,
    FR_REF_SOURCE, FR_REF_RATE, FR_TASK, FR_SEQUENCE, FR_BOARD_VERSION,
    FR_BOARD_FLAGS, FR_SOURCE, FR_REPEAT, FR_RESERVED
};
void board_final_rf_snapshot(u32 words[FINAL_RF_STATUS_WORDS]);

/* RDFQ+nonce -> RDFA+nonce+20个小端u32：最近一次屏幕/PC整机请求的原始值，
 * 不论PS是否接受。屏幕或PC修改参数后即使保护拒绝发射，另一侧也能看到请求值；
 * result字段明确给出PS结果，未应用的值不得当作已发射状态。 */
#define FINAL_RF_REQUEST_VERSION 1U
#define FINAL_RF_REQUEST_WORDS 20U
enum {
    FRQ_VERSION, FRQ_VALID, FRQ_SEQUENCE, FRQ_OP, FRQ_RESULT,
    FRQ_DM, FRQ_DR, FRQ_DC, FRQ_DFR, FRQ_DB, FRQ_DN, FRQ_DI,
    FRQ_DSL, FRQ_DSH, FRQ_DSX, FRQ_DSB, FRQ_DF, FRQ_DPO,
    FRQ_RESERVED0, FRQ_RESERVED1
};
void board_final_rf_request_snapshot(u32 words[FINAL_RF_REQUEST_WORDS]);
#endif
