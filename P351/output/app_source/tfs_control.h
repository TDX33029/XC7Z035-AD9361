#ifndef TFS_CONTROL_H
#define TFS_CONTROL_H
#include "xparameters.h"
#include "xil_io.h"
#include "xil_types.h"
/* Old XSA/BSP must never probe a guessed AXI address. New BD maps this core
 * at 0x43C40000, distinct from the retired PRBS calibration peripheral. */
#if defined(XPAR_TFS_SPI_0_S_AXI_BASEADDR)
#define TFS_BASE XPAR_TFS_SPI_0_S_AXI_BASEADDR
#elif defined(XPAR_TFS_SPI_0_BASEADDR)
#define TFS_BASE XPAR_TFS_SPI_0_BASEADDR
#endif

/* Result: bits0..3 PL flags; bit8 peripheral present; bits16..23 error:
 * 1 missing hardware, 2 invalid argument, 3 busy. No polling/lock wait. */
static inline u32 tfs_control_status(void)
{
#ifdef TFS_BASE
    u32 id = Xil_In32(TFS_BASE);
    if (id == 0x54465301U || id == 0x54465302U) {
        u32 s = 0x100U | (Xil_In32(TFS_BASE + 8U) & 15U);
        if (id == 0x54465302U)
            s |= 0x200U | ((Xil_In32(TFS_BASE+0x28U) & 127U) << 24);
        return s;
    }
#endif
    return 1U << 16;
}

static inline u32 tfs_control_request(u8 op, u16 half_period, u64 value)
{
    u32 status = tfs_control_status();
    if (!(status & 0x100U)) return status;
    if (op == 0U) return status;
    if (op > 5U && op != 0x81U && op != 0x82U && op != 0x86U)
        return status | (2U << 16);
#ifdef TFS_BASE
    if (op == 4U || op == 5U) {
        u32 levels;
        if (!(status & 0x200U)) return status | (4U << 16);
        if (op == 5U) return status;
        if (value > 126U) return status | (2U << 16);
        /* Unit=0.5 dB. V7 weighs 63 units, not 64. */
        levels = value >= 63U ? ((~((u32)value-63U)) & 63U) :
                               (64U | ((~(u32)value) & 63U));
        Xil_Out32(TFS_BASE+0x28U, levels);
        status=tfs_control_status();
        if (((status >> 24) & 127U) != levels) status |= 5U << 16;
        return status;
    }
    if (op == 3U) {
        Xil_Out32(TFS_BASE + 4U, 512U);
        return tfs_control_status();
    }
    /* 100 MHz AXI / (2 * 5) = 10 MHz; cap at recommended speed. */
    if (half_period < 5U ||
        (op == 1U && (value < 500000000000ULL || value > 226000000000000ULL)) ||
        (op == 2U && (value < 500000000ULL || value > 226000000000ULL)))
        return status | (2U << 16);
    if (status & 1U) return status | (3U << 16);
    Xil_Out32(TFS_BASE + 12U, half_period);
    Xil_Out32(TFS_BASE + 16U, (u32)value);
    Xil_Out32(TFS_BASE + 20U, (u32)(value >> 32));
    Xil_Out32(TFS_BASE + 4U, op);
    return tfs_control_status();
#else
    (void)half_period; (void)value;
    return status;
#endif
}
/* Snapshot raw response plus opcode; foreground serializes access. */
static inline void tfs_control_snapshot(u32 words[4])
{
    words[0]=words[1]=words[2]=words[3]=0U;
#ifdef TFS_BASE
    if (tfs_control_status() & 0x100U) {
        words[0]=Xil_In32(TFS_BASE+0x18U);
        words[1]=Xil_In32(TFS_BASE+0x1cU);
        words[2]=Xil_In32(TFS_BASE+0x20U);
        words[3]=Xil_In32(TFS_BASE+0x24U);
    }
#endif
}
#endif
