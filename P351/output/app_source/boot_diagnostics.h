#ifndef AD9361TX_BOOT_DIAGNOSTICS_H
#define AD9361TX_BOOT_DIAGNOSTICS_H

/* Diagnostics only: never modifies boot straps, Flash, or RF state. */
static void print_boot_diagnostics(void)
{
    const u32 boot_mode = Xil_In32(0xF800025CU) & 0xFU; /* Zynq SLCR BOOT_MODE */
    const char *source = "OTHER";
    if (boot_mode == 0U) source = "JTAG";
    else if (boot_mode == 1U) source = "QSPI_FLASH";
    else if (boot_mode == 5U) source = "SD";
#if P351_MANUAL_RF_TEST
    xil_printf("[boot] P351-MANUAL-DIRECT-r10 build=%s %s straps=%s(0x%X)\r\n",
               __DATE__, __TIME__, source, (unsigned)boot_mode);
#else
    xil_printf("[boot] P351-OFFLINE-CANDIDATE build=%s %s straps=%s(0x%X)\r\n",
               __DATE__, __TIME__, source, (unsigned)boot_mode);
#endif
#if P351_MANUAL_RF_TEST
    xil_printf("[boot] P351 MANUAL RF software permission; full attenuation range, NOT hardware acceptance\r\n");
#elif P351_RF_HARDWARE_QUALIFIED
    xil_printf("[boot] P351 RF software permission enabled; physical RF requires acceptance tests\r\n");
#else
    xil_printf("[boot] P351 protected candidate; RF software permission OFF\r\n");
#endif
#if defined(XPAR_PS7_QSPI_LINEAR_0_S_AXI_BASEADDR)
#if BOOT_PCM_FM_ENABLED
    xil_printf("[boot] BSP QSPI enabled; auto PCM/FM after initialization and safety checks\r\n");
#else
    xil_printf("[boot] BSP QSPI present; auto-TX OFF; manual TX requires reference/PLL/LVDS/atten checks\r\n");
#endif
#else
    xil_printf("[boot] BSP QSPI absent: JTAG test only, rebuild platform for Flash boot\r\n");
#endif
    /* The strap reports the selected boot mode, not proof of image provenance.
     * A standalone cold boot without a debugger is the final acceptance test. */
}

#endif
